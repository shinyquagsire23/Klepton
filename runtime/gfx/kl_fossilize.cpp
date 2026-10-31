// kl_fossilize.cpp — a guest's Vulkan pipelines, recorded and replayed with
// Valve's Fossilize (vendor-fossilize/, MIT).
//
// On the headset a pipeline the Metal compiler has not seen costs 300-600 ms,
// and Source 2 creates them one at a time as new views appear. The system's
// Metal cache keeps what it has compiled across launches, so the cure is to
// have compiled everything BEFORE the guest asks:
//
//   KL_VK_FOZ_RECORD=<file.foz>|auto   record every sampler, set and pipeline
//       layout, shader module, render pass and pipeline the guest creates (its
//       own create infos), appending to a Fossilize stream archive ("auto":
//       recorded.foz in KL_VK_FOZ_DIR, else $HOME/Documents/klfz).
//   KL_VK_FOZ_REPLAY=<file.foz|dir>[:...]|auto   replay those databases on
//       worker threads (KL_VK_FOZ_THREADS, default 4) through the layer's own
//       entry points, so each pipeline goes through the same fix-ups and
//       MoltenVK state as the guest's and produces the same MSL, which is what
//       the Metal cache is keyed on. Each replayed pipeline is destroyed at
//       once; only the warm cache is kept. A pipeline that an earlier database
//       of the same replay held is not compiled a second time.
//   KL_VK_FOZ_APP=<name>   the application (by its application or its engine
//       name) whose device is replayed on and recorded from, where a process
//       holds more than one: an OpenVR runtime makes a device of its own
//       before the game's.
//   KL_VK_FOZ_REPLAY_BLOCK=1   the guest's vkCreateDevice returns when the
//       replay is done, instead of racing it into its first views.
//
// kl_foz_prewarm does the same with no guest at all, on a device made as the
// game makes its own, for a launcher to run before it starts the game.
//
// The method — and most of this file — is Glovebox's (github.com/glovebox-vr/
// glovebox, MIT, klepton/runtime/linux/kl_fossilize.cpp). What differs here:
// databases are replayed in name order and a pipeline is compiled once however
// many of them hold it; the application filter; a replay before the game is not
// repeated at the game's own device when it ran on the game's own settings; and
// the prewarm device is given back through the layer's own destroy calls.
#include "fossilize.hpp"
#include "fossilize_db.hpp"
#include <vulkan/vulkan.h>
#include "kl_vk_pipe_hooks.h"
#include "rapidjson/document.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>
#include <dirent.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

using namespace Fossilize;

extern "C" {
void kl_fossilize_install(void);
int kl_foz_progress(unsigned *done, unsigned *total, double *elapsed);
int kl_foz_prewarm(const char *info_path);
int kl_foz_databases(void);
void *kl_vulkan_sym(const char *name);
}

namespace {

double now_s() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

bool env_on(const char *name) {
    const char *e = getenv(name);
    return e && *e && strcmp(e, "0");
}

std::string default_dir() {
    const char *d = getenv("KL_VK_FOZ_DIR");
    if (d && *d) return d;
    const char *h = getenv("HOME");
    return std::string(h && *h ? h : ".") + "/Documents/klfz";
}

// The device this is about: KL_VK_FOZ_APP unset means every device.
bool app_wanted() { return klvk_instance_wanted() != 0; }

// ---- recording ---------------------------------------------------------------------------
StateRecorder *g_rec;
DatabaseInterface *g_rec_db;
std::atomic<unsigned> g_rec_pipes{0}, g_rec_fail{0};

// Replay progress for a launcher (kl_foz_progress): pipelines to compile, pipelines compiled (or failed), and the
// state: 0 no replay, 1 replaying, 2 done.
std::atomic<unsigned> g_prog_total{0}, g_prog_done{0};
std::atomic<int> g_prog_state{0};
std::atomic<double> g_prog_t0{0};
// Over every database of one replay: pipelines compiled, and those that took over 100 ms (which is what a cold cache
// looks like — a warm one answers in a few).
std::atomic<unsigned> g_sum_compiled{0}, g_sum_slow{0};

void rec_init() {
    const char *e = getenv("KL_VK_FOZ_RECORD");
    if (!e || !*e || !strcmp(e, "0")) return;
    std::string path = e;
    if (path == "1" || path == "auto") {
        path = default_dir();
        mkdir(path.c_str(), 0755);
        path += "/recorded.foz";
    }
    g_rec_db = create_stream_archive_database(path.c_str(), DatabaseMode::Append);
    if (!g_rec_db || !g_rec_db->prepare()) {
        fprintf(stderr, "  [foz] cannot open %s for recording; recording off\n", path.c_str());
        delete g_rec_db;
        g_rec_db = nullptr;
        return;
    }
    g_rec = new StateRecorder;
    g_rec->set_database_enable_compression(true);
    g_rec->set_database_enable_checksum(true);
    const char *an = getenv("KL_VK_FOZ_APP");
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, an && *an ? an : "guest", 1, "klepton", 1,
                              VK_API_VERSION_1_1 };
    if (!g_rec->record_application_info(app)) fprintf(stderr, "  [foz] application info not recorded\n");
    // Synchronized by default: each record is written and flushed before the create returns, so a run that is ended
    // from outside — which is how every run on a headset ends — loses nothing. KL_VK_FOZ_RECORD_SYNC=0 hands the
    // writes to a thread.
    const char *sy = getenv("KL_VK_FOZ_RECORD_SYNC");
    bool sync = !(sy && !strcmp(sy, "0"));
    if (sync) g_rec->init_recording_synchronized(g_rec_db);
    else g_rec->init_recording_thread(g_rec_db);
    fprintf(stderr, "  [foz] recording the guest's pipelines to %s (%s)\n", path.c_str(),
            sync ? "synchronized: flushed per record" : "recording thread");
}

void note_pipe(bool ok) {
    unsigned n = ok ? ++g_rec_pipes : g_rec_pipes.load();
    if (!ok) ++g_rec_fail;
    if (ok && (n & (n - 1)) == 0 && n >= 64)
        fprintf(stderr, "  [foz] %u pipelines recorded (%u refused)\n", n, g_rec_fail.load());
}

// Set while kl_foz_prewarm has its own device: nothing of it is the guest's.
std::atomic<bool> g_prewarming{false};
bool rec_on() { return g_rec && !g_prewarming.load() && app_wanted(); }

void h_sampler(VkSampler h, const VkSamplerCreateInfo *ci) { if (rec_on()) (void)g_rec->record_sampler(h, *ci); }
void h_set_layout(VkDescriptorSetLayout h, const VkDescriptorSetLayoutCreateInfo *ci) { if (rec_on()) (void)g_rec->record_descriptor_set_layout(h, *ci); }
void h_pipeline_layout(VkPipelineLayout h, const VkPipelineLayoutCreateInfo *ci) { if (rec_on()) (void)g_rec->record_pipeline_layout(h, *ci); }
void h_shader_module(VkShaderModule h, const VkShaderModuleCreateInfo *ci) { if (rec_on()) (void)g_rec->record_shader_module(h, *ci); }
void h_render_pass(VkRenderPass h, const VkRenderPassCreateInfo *ci) { if (rec_on()) (void)g_rec->record_render_pass(h, *ci); }
void h_render_pass2(VkRenderPass h, const VkRenderPassCreateInfo2 *ci) { if (rec_on()) (void)g_rec->record_render_pass2(h, *ci); }
void h_graphics_pipeline(VkPipeline h, const VkGraphicsPipelineCreateInfo *ci) {
    if (rec_on()) note_pipe(g_rec->record_graphics_pipeline(h, *ci, nullptr, 0));
}
void h_compute_pipeline(VkPipeline h, const VkComputePipelineCreateInfo *ci) {
    if (rec_on()) note_pipe(g_rec->record_compute_pipeline(h, *ci, nullptr, 0));
}

// ---- replay ------------------------------------------------------------------------------
struct Fns {
    PFN_vkCreateSampler CreateSampler;
    PFN_vkDestroySampler DestroySampler;
    PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout;
    PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout;
    PFN_vkCreatePipelineLayout CreatePipelineLayout;
    PFN_vkDestroyPipelineLayout DestroyPipelineLayout;
    PFN_vkCreateShaderModule CreateShaderModule;
    PFN_vkDestroyShaderModule DestroyShaderModule;
    PFN_vkCreateRenderPass CreateRenderPass;
    PFN_vkCreateRenderPass2 CreateRenderPass2;
    PFN_vkDestroyRenderPass DestroyRenderPass;
    PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines;
    PFN_vkCreateComputePipelines CreateComputePipelines;
    PFN_vkDestroyPipeline DestroyPipeline;
};

struct Job { bool graphics; const void *ci; };

// Creates the dependency objects inline (the parse thread) and queues the pipelines for the compile threads.
// Fossilize keeps every parsed create info alive while the StateReplayer lives.
struct Creator : StateCreatorInterface {
    VkDevice dev;
    const Fns &f;
    std::vector<VkSampler> samplers;
    std::vector<VkDescriptorSetLayout> set_layouts;
    std::vector<VkPipelineLayout> layouts;
    std::vector<VkShaderModule> modules;
    std::vector<VkRenderPass> passes;
    std::mutex mu;
    std::condition_variable cv;
    std::deque<Job> queue;
    bool closed = false;
    unsigned compiled = 0, failed = 0, slow = 0;
    double compile_s = 0;

    Creator(VkDevice d, const Fns &fns) : dev(d), f(fns) {}
    unsigned dep_fails = 0;
    void dep_fail(const char *what, VkResult r) {
        if (dep_fails++ < 4) fprintf(stderr, "  [foz] replay: creating a %s failed (VkResult %d)\n", what, (int)r);
    }

    bool enqueue_create_sampler(Hash, const VkSamplerCreateInfo *ci, VkSampler *out) override {
        *out = VK_NULL_HANDLE;
        if (VkResult r = f.CreateSampler(dev, ci, nullptr, out)) { dep_fail("sampler", r); return false; }
        samplers.push_back(*out);
        return true;
    }
    bool enqueue_create_descriptor_set_layout(Hash, const VkDescriptorSetLayoutCreateInfo *ci, VkDescriptorSetLayout *out) override {
        *out = VK_NULL_HANDLE;
        if (VkResult r = f.CreateDescriptorSetLayout(dev, ci, nullptr, out)) { dep_fail("set layout", r); return false; }
        set_layouts.push_back(*out);
        return true;
    }
    bool enqueue_create_pipeline_layout(Hash, const VkPipelineLayoutCreateInfo *ci, VkPipelineLayout *out) override {
        *out = VK_NULL_HANDLE;
        if (VkResult r = f.CreatePipelineLayout(dev, ci, nullptr, out)) { dep_fail("pipeline layout", r); return false; }
        layouts.push_back(*out);
        return true;
    }
    bool enqueue_create_shader_module(Hash, const VkShaderModuleCreateInfo *ci, VkShaderModule *out) override {
        *out = VK_NULL_HANDLE;
        if (VkResult r = f.CreateShaderModule(dev, ci, nullptr, out)) { dep_fail("shader module", r); return false; }
        modules.push_back(*out);
        return true;
    }
    bool enqueue_create_render_pass(Hash, const VkRenderPassCreateInfo *ci, VkRenderPass *out) override {
        *out = VK_NULL_HANDLE;
        if (VkResult r = f.CreateRenderPass(dev, ci, nullptr, out)) { dep_fail("render pass", r); return false; }
        passes.push_back(*out);
        return true;
    }
    bool enqueue_create_render_pass2(Hash, const VkRenderPassCreateInfo2 *ci, VkRenderPass *out) override {
        *out = VK_NULL_HANDLE;
        if (!f.CreateRenderPass2 || f.CreateRenderPass2(dev, ci, nullptr, out) != VK_SUCCESS) return false;
        passes.push_back(*out);
        return true;
    }
    void push(bool graphics, const void *ci) {
        std::lock_guard<std::mutex> l(mu);
        queue.push_back({ graphics, ci });
        cv.notify_one();
    }
    bool enqueue_create_graphics_pipeline(Hash, const VkGraphicsPipelineCreateInfo *ci, VkPipeline *out) override {
        *out = VK_NULL_HANDLE;
        push(true, ci);
        return true;
    }
    bool enqueue_create_compute_pipeline(Hash, const VkComputePipelineCreateInfo *ci, VkPipeline *out) override {
        *out = VK_NULL_HANDLE;
        push(false, ci);
        return true;
    }
    bool enqueue_create_raytracing_pipeline(Hash, const VkRayTracingPipelineCreateInfoKHR *, VkPipeline *out) override {
        *out = VK_NULL_HANDLE;
        return true;
    }

    void worker() {
        klvk_set_replay_thread(1);
        for (;;) {
            Job j;
            {
                std::unique_lock<std::mutex> l(mu);
                cv.wait(l, [&] { return closed || !queue.empty(); });
                if (queue.empty()) return;
                j = queue.front();
                queue.pop_front();
            }
            double t0 = now_s();
            VkPipeline p = VK_NULL_HANDLE;
            VkResult r = j.graphics
                ? f.CreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, (const VkGraphicsPipelineCreateInfo *)j.ci, nullptr, &p)
                : f.CreateComputePipelines(dev, VK_NULL_HANDLE, 1, (const VkComputePipelineCreateInfo *)j.ci, nullptr, &p);
            double dt = now_s() - t0;
            if (p) f.DestroyPipeline(dev, p, nullptr);
            std::lock_guard<std::mutex> l(mu);
            compiled++;
            g_prog_done.fetch_add(1, std::memory_order_relaxed);
            compile_s += dt;
            if (dt > 0.1) slow++;
            if (r != VK_SUCCESS) failed++;
        }
    }
    void destroy_objects() {
        for (auto h : modules) f.DestroyShaderModule(dev, h, nullptr);
        for (auto h : layouts) f.DestroyPipelineLayout(dev, h, nullptr);
        for (auto h : set_layouts) f.DestroyDescriptorSetLayout(dev, h, nullptr);
        for (auto h : passes) f.DestroyRenderPass(dev, h, nullptr);
        for (auto h : samplers) f.DestroySampler(dev, h, nullptr);
    }
};

// The databases of KL_VK_FOZ_REPLAY, a folder's in name order — so a list that came with the game ("alyx.foz") is
// replayed before what this device recorded beside it ("recorded.foz"), and the second is left with what is new.
std::vector<std::string> replay_files() {
    std::vector<std::string> out;
    const char *e = getenv("KL_VK_FOZ_REPLAY");
    if (!e || !*e || !strcmp(e, "0")) return out;
    std::string list = (!strcmp(e, "1") || !strcmp(e, "auto")) ? default_dir() : e;
    size_t pos = 0;
    while (pos <= list.size()) {
        size_t c = list.find(':', pos);
        std::string item = list.substr(pos, c == std::string::npos ? std::string::npos : c - pos);
        pos = c == std::string::npos ? list.size() + 1 : c + 1;
        if (item.empty()) continue;
        struct stat st;
        if (stat(item.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            std::vector<std::string> in;
            if (DIR *d = opendir(item.c_str())) {
                while (struct dirent *de = readdir(d)) {
                    size_t n = strlen(de->d_name);
                    if (n > 4 && !strcmp(de->d_name + n - 4, ".foz")) in.push_back(item + "/" + de->d_name);
                }
                closedir(d);
            }
            std::sort(in.begin(), in.end());
            out.insert(out.end(), in.begin(), in.end());
        } else if (st.st_size > 0) {
            out.push_back(item);
        }
    }
    return out;
}

struct Replay {
    // pipelines an earlier database of this replay already held
    std::unordered_set<Hash> seen_graphics, seen_compute;
};

void replay_db(VkDevice dev, const Fns &f, const std::string &path, unsigned nthreads, bool urgent, Replay &all) {
    DatabaseInterface *db = create_stream_archive_database(path.c_str(), DatabaseMode::ReadOnly);
    if (!db || !db->prepare()) {
        fprintf(stderr, "  [foz] replay: cannot open %s\n", path.c_str());
        delete db;
        return;
    }
    double t0 = now_s();
    // What of this database is new to the replay; one that holds nothing new costs nothing.
    std::vector<Hash> fresh[2];
    unsigned held = 0;
    for (int k = 0; k < 2; k++) {
        ResourceTag tag = k ? RESOURCE_COMPUTE_PIPELINE : RESOURCE_GRAPHICS_PIPELINE;
        auto &seen = k ? all.seen_compute : all.seen_graphics;
        size_t n = 0;
        if (!db->get_hash_list_for_resource_tag(tag, &n, nullptr)) continue;
        std::vector<Hash> hashes(n);
        if (!db->get_hash_list_for_resource_tag(tag, &n, hashes.data())) continue;
        held += (unsigned)n;
        for (Hash h : hashes)
            if (seen.insert(h).second) fresh[k].push_back(h);
    }
    unsigned todo = (unsigned)(fresh[0].size() + fresh[1].size());
    if (!todo) {
        fprintf(stderr, "  [foz] replay %s: its %u pipelines were in the databases before it\n", path.c_str(), held);
        delete db;
        return;
    }
    g_prog_total.fetch_add(todo, std::memory_order_relaxed);

    StateReplayer replayer;
    Creator creator(dev, f);
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < nthreads; i++) workers.emplace_back([&] {
        pthread_setname_np("klfz replay");
        // Before the game the player is waiting for this and nothing else; behind a running game it must not be
        // what the game waits for.
        pthread_set_qos_class_self_np(urgent ? QOS_CLASS_USER_INITIATED : QOS_CLASS_UTILITY, 0);
        creator.worker();
    });
    unsigned parse_fail = 0;
    std::vector<uint8_t> buf;
    auto parse = [&](ResourceTag tag, Hash h) {
        size_t sz = 0;
        if (!db->read_entry(tag, h, &sz, nullptr, PAYLOAD_READ_NO_FLAGS)) { parse_fail++; return; }
        buf.resize(sz);
        if (!db->read_entry(tag, h, &sz, buf.data(), PAYLOAD_READ_NO_FLAGS)) { parse_fail++; return; }
        if (!replayer.parse(creator, db, buf.data(), sz)) parse_fail++;
    };
    // Dependencies first: the replayer resolves pipeline layouts and render passes only from objects already
    // replayed (shader modules and samplers it can also pull from the database).
    for (ResourceTag tag : { RESOURCE_SAMPLER, RESOURCE_DESCRIPTOR_SET_LAYOUT, RESOURCE_PIPELINE_LAYOUT,
                             RESOURCE_RENDER_PASS, RESOURCE_SHADER_MODULE }) {
        size_t n = 0;
        if (!db->get_hash_list_for_resource_tag(tag, &n, nullptr)) continue;
        std::vector<Hash> hashes(n);
        if (!db->get_hash_list_for_resource_tag(tag, &n, hashes.data())) continue;
        for (Hash h : hashes) parse(tag, h);
    }
    for (Hash h : fresh[0]) parse(RESOURCE_GRAPHICS_PIPELINE, h);
    for (Hash h : fresh[1]) parse(RESOURCE_COMPUTE_PIPELINE, h);
    {
        std::lock_guard<std::mutex> l(creator.mu);
        creator.closed = true;
        creator.cv.notify_all();
    }
    for (auto &t : workers) t.join();
    creator.destroy_objects();
    delete db;
    // what could not be parsed never reached a worker: count it done, or a progress bar stops short of its end
    if (todo > creator.compiled) g_prog_done.fetch_add(todo - creator.compiled, std::memory_order_relaxed);
    fprintf(stderr, "  [foz] replay %s: %u pipelines (%u failed, %u unparsed, %u in the databases before it) in %.1f s "
                    "on %u threads; %.1f s of compiles, %u over 100 ms\n",
            path.c_str(), creator.compiled, creator.failed, parse_fail, held - todo, now_s() - t0, nthreads,
            creator.compile_s, creator.slow);
    g_sum_compiled.fetch_add(creator.compiled);
    g_sum_slow.fetch_add(creator.slow);
}

// A replay before the game that ran on the game's own device settings: the game's own device need not repeat it.
std::atomic<bool> g_prewarmed_exactly{false};

void start_replay(VkDevice dev, PFN_vkGetDeviceProcAddr gdpa, bool block);

void h_device_created(VkDevice dev, PFN_vkGetDeviceProcAddr gdpa) {
    if (g_prewarming.load() || !app_wanted()) return;
    if (g_prewarmed_exactly.load() && !env_on("KL_VK_FOZ_REPLAY_TWICE")) {
        fprintf(stderr, "  [foz] the databases were replayed before the game on a device made as this one; not again\n");
        return;
    }
    start_replay(dev, gdpa, env_on("KL_VK_FOZ_REPLAY_BLOCK"));
}

void start_replay(VkDevice dev, PFN_vkGetDeviceProcAddr gdpa, bool block) {
    std::vector<std::string> files = replay_files();
    if (files.empty()) return;
    static Fns f;
#define L(n) f.n = (PFN_vk##n)gdpa(dev, "vk" #n)
    L(CreateSampler); L(DestroySampler); L(CreateDescriptorSetLayout); L(DestroyDescriptorSetLayout);
    L(CreatePipelineLayout); L(DestroyPipelineLayout); L(CreateShaderModule); L(DestroyShaderModule);
    L(CreateRenderPass); L(CreateRenderPass2); L(DestroyRenderPass);
    L(CreateGraphicsPipelines); L(CreateComputePipelines); L(DestroyPipeline);
#undef L
    unsigned nthreads = 4;
    if (const char *t = getenv("KL_VK_FOZ_THREADS")) nthreads = (unsigned)atoi(t);
    if (nthreads < 1) nthreads = 1;
    fprintf(stderr, "  [foz] replaying %zu database(s) on %u threads to warm the Metal cache%s\n",
            files.size(), nthreads, block ? " (held until done)" : " in the background");
    auto run = [dev, files, nthreads, block] {
        pthread_setname_np("klfz replay main");
        klvk_set_replay_thread(1);
        if (!block) pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
        double t0 = now_s();
        g_prog_total.store(0); g_prog_done.store(0);
        g_prog_t0.store(t0); g_prog_state.store(1);
        Replay all;
        for (const auto &p : files) replay_db(dev, f, p, nthreads, block, all);
        g_prog_state.store(2);
        fprintf(stderr, "  [foz] replay done in %.1f s\n", now_s() - t0);
        klvk_set_replay_thread(0);
    };
    if (block) run();
    else std::thread(run).detach();
}

// The guest's device features, in the recording's application info (so a recording says which device its pipelines
// were made for). Before the device exists, so before anything is recorded against the application info.
void h_device_info(const VkDeviceCreateInfo *ci) {
    if (!g_rec || g_prewarming.load()) return;
    bool ok = ci->pEnabledFeatures ? g_rec->record_physical_device_features(*ci->pEnabledFeatures)
                                   : g_rec->record_physical_device_features(ci->pNext);
    if (!ok) fprintf(stderr, "  [foz] device features not recorded\n");
}

const klvk_pipe_hooks g_hooks = {
    h_device_created, h_sampler, h_set_layout, h_pipeline_layout, h_shader_module,
    h_render_pass, h_render_pass2, h_graphics_pipeline, h_compute_pipeline, h_device_info,
};

} // namespace

// A replay's progress, for a launcher: returns 0 (no replay yet), 1 (replaying) or 2 (done); `done`/`total` are
// pipelines compiled and to compile (the total grows as each database is opened), `elapsed` the seconds since it began.
extern "C" int kl_foz_progress(unsigned *done, unsigned *total, double *elapsed) {
    int st = g_prog_state.load();
    if (done) *done = g_prog_done.load();
    if (total) *total = g_prog_total.load();
    if (elapsed) *elapsed = st ? now_s() - g_prog_t0.load() : 0;
    return st;
}

// How many databases a replay would read now.
extern "C" int kl_foz_databases(void) { return (int)replay_files().size(); }

extern "C" void kl_fossilize_install(void) {
    static bool done;
    if (done) return;
    done = true;
    if (!env_on("KL_VK_FOZ_RECORD") && !env_on("KL_VK_FOZ_REPLAY")) return;
    if (env_on("KL_VK_FOZ_RECORD")) rec_init();
    klvk_set_pipe_hooks(&g_hooks);
}

// Warm the Metal cache before the game starts, with no guest at all: make an instance and a device exactly as the game
// made its own (device-info.json, which the layer writes at the game's vkCreateDevice: KL_VK_DEVICE_INFO), through the
// layer's own entry points, and replay the databases on it (KL_VK_FOZ_REPLAY), blocking. Progress is kl_foz_progress's.
// Returns 0 when it replayed; -2 when the device info is missing or not usable (try another copy), -1 when it could
// not otherwise (no databases, a create that failed). The game's own replay is the fallback.
extern "C" int kl_foz_prewarm(const char *info_path) {
    kl_fossilize_install();
    if (replay_files().empty()) return -1;
    if (!info_path) return -2;
    std::ifstream in(info_path);
    if (!in) return -2;
    std::stringstream ss; ss << in.rdbuf();
    std::string text = ss.str();
    rapidjson::Document d;
    d.Parse(text.c_str());
    if (d.HasParseError() || !d.IsObject() || !d.HasMember("instance") || !d.HasMember("device")) {
        fprintf(stderr, "  [foz] prewarm: %s is not device info\n", info_path);
        return -2;
    }
    auto hex = [](const char *h, std::vector<uint8_t> &out) {
        size_t n = strlen(h) / 2;
        out.resize(n);
        for (size_t i = 0; i < n; i++) { unsigned v = 0; sscanf(h + 2 * i, "%2x", &v); out[i] = (uint8_t)v; }
    };
    auto gipa = (PFN_vkGetInstanceProcAddr)kl_vulkan_sym("vkGetInstanceProcAddr");
    if (!gipa) return -1;
    const auto &ji = d["instance"], &jd = d["device"];
    if (!ji.IsObject() || !jd.IsObject() || !ji.HasMember("extensions") || !jd.HasMember("extensions") ||
        !jd.HasMember("queues") || !jd.HasMember("pnext") || !jd.HasMember("enabled_features")) return -2;

    std::string app_name = ji.HasMember("app") && ji["app"].IsString() ? ji["app"].GetString() : "";
    std::string eng_name = ji.HasMember("engine") && ji["engine"].IsString() ? ji["engine"].GetString() : "";
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = app_name.c_str();
    app.applicationVersion = ji.HasMember("app_version") ? ji["app_version"].GetUint() : 0;
    app.pEngineName = eng_name.c_str();
    app.engineVersion = ji.HasMember("engine_version") ? ji["engine_version"].GetUint() : 0;
    app.apiVersion = ji.HasMember("api") ? ji["api"].GetUint() : VK_API_VERSION_1_1;
    std::vector<const char *> iext;
    for (const auto &e : ji["extensions"].GetArray()) iext.push_back(e.GetString());
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = (uint32_t)iext.size(); ici.ppEnabledExtensionNames = iext.data();

    std::vector<const char *> dext;
    for (const auto &e : jd["extensions"].GetArray()) dext.push_back(e.GetString());
    std::vector<VkDeviceQueueCreateInfo> qs;
    static const float prio[16] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
    for (const auto &q : jd["queues"].GetArray()) {
        VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        qi.queueFamilyIndex = q["family"].GetUint();
        qi.queueCount = std::min(q["count"].GetUint(), 16u);
        qi.flags = q["flags"].GetUint();
        qi.pQueuePriorities = prio;
        qs.push_back(qi);
    }
    VkPhysicalDeviceFeatures feats = {};
    bool have_feats = jd["enabled_features"].IsString();
    if (have_feats) {
        std::vector<uint8_t> b; hex(jd["enabled_features"].GetString(), b);
        if (b.size() != sizeof feats) { fprintf(stderr, "  [foz] prewarm: enabled_features size %zu\n", b.size()); return -2; }
        memcpy(&feats, b.data(), sizeof feats);
    }
    std::vector<std::vector<uint8_t>> chain;
    for (const auto &st : jd["pnext"].GetArray()) {
        size_t size = st["size"].GetUint();
        if (size <= sizeof(VkBaseOutStructure)) {
            fprintf(stderr, "  [foz] prewarm: device info has a feature struct (sType %u) this build cannot rebuild\n",
                    st["sType"].GetUint());
            return -2;
        }
        std::vector<uint8_t> b(size, 0), data;
        hex(st["data"].GetString(), data);
        if (data.size() != size - sizeof(VkBaseOutStructure)) return -2;
        ((VkBaseOutStructure *)b.data())->sType = (VkStructureType)st["sType"].GetUint();
        memcpy(b.data() + sizeof(VkBaseOutStructure), data.data(), data.size());
        chain.push_back(std::move(b));
    }
    for (size_t i = 0; i + 1 < chain.size(); i++)
        ((VkBaseOutStructure *)chain[i].data())->pNext = (VkBaseOutStructure *)chain[i + 1].data();
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.pNext = chain.empty() ? nullptr : chain[0].data();
    dci.queueCreateInfoCount = (uint32_t)qs.size(); dci.pQueueCreateInfos = qs.data();
    dci.enabledExtensionCount = (uint32_t)dext.size(); dci.ppEnabledExtensionNames = dext.data();
    dci.pEnabledFeatures = have_feats ? &feats : nullptr;

    // the layer would write this device's settings over the game's
    std::string keep_info = getenv("KL_VK_DEVICE_INFO") ? getenv("KL_VK_DEVICE_INFO") : "";
    unsetenv("KL_VK_DEVICE_INFO");
    double t0 = now_s();
    VkInstance inst = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    g_prewarming.store(true);
    VkResult r = ((PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance"))(&ici, nullptr, &inst);
    if (r == VK_SUCCESS) {
        uint32_t n = 1;
        VkPhysicalDevice phys = VK_NULL_HANDLE;
        ((PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices"))(inst, &n, &phys);
        r = phys ? ((PFN_vkCreateDevice)gipa(inst, "vkCreateDevice"))(phys, &dci, nullptr, &dev) : VK_ERROR_INITIALIZATION_FAILED;
    }
    if (!keep_info.empty()) setenv("KL_VK_DEVICE_INFO", keep_info.c_str(), 1);
    if (r != VK_SUCCESS) {
        if (inst) ((PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance"))(inst, nullptr);
        g_prewarming.store(false);
        fprintf(stderr, "  [foz] prewarm: could not make the device (%d)\n", (int)r);
        return -1;
    }
    fprintf(stderr, "  [foz] prewarm: device made as %s (%zu + %zu extensions, %zu feature structs); replaying\n",
            info_path, iext.size(), dext.size(), chain.size());
    g_sum_compiled.store(0); g_sum_slow.store(0);
    auto gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
    start_replay(dev, gdpa, true);
    // Given back through the layer, which keeps its own books of devices and instances: the game's device has to be
    // the only one it knows when the game makes it.
    ((PFN_vkDestroyDevice)gipa(inst, "vkDestroyDevice"))(dev, nullptr);
    ((PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance"))(inst, nullptr);
    g_prewarming.store(false);
    fprintf(stderr, "  [foz] prewarm done in %.1f s: %u pipelines, %u of them over 100 ms\n", now_s() - t0,
            g_sum_compiled.load(), g_sum_slow.load());
    return 0;
}

// The same, and that it ran on the game's own settings (see h_device_created).
extern "C" void kl_foz_prewarm_exact(int exact) { g_prewarmed_exactly.store(exact != 0); }
