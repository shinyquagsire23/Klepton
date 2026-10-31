#include "main.hpp"

#include <GLES3/gl32.h>
#include <android/bitmap.h>

#include <map>

#include "GlobalNamespace/MainFlowCoordinator.hpp"
#include "System/Collections/Generic/Dictionary_2.hpp"
#include "System/IntPtr.hpp"
#include "TMPro/TMP_FontAsset.hpp"
#include "TMPro/TMP_FontAssetUtilities.hpp"
#include "TMPro/TMP_Settings.hpp"
#include "TMPro/TMP_Sprite.hpp"
#include "TMPro/TMP_SpriteAsset.hpp"
#include "TMPro/TMP_SpriteCharacter.hpp"
#include "TMPro/TMP_SpriteGlyph.hpp"
#include "TMPro/TMP_Text.hpp"
#include "TMPro/TMP_TextInfo.hpp"
#include "UnityEngine/Color.hpp"
#include "UnityEngine/Font.hpp"
#include "UnityEngine/Material.hpp"
#include "UnityEngine/Resources.hpp"
#include "UnityEngine/Texture2D.hpp"
#include "UnityEngine/TextureFormat.hpp"
#include "beatsaber-hook/shared/utils/hooking.hpp"
#include "beatsaber-hook/shared/utils/il2cpp-utils.hpp"
#include "metacore/shared/java.hpp"
#include "metacore/shared/unity.hpp"

static modloader::ModInfo modInfo = {MOD_ID, VERSION, 0};

using namespace UnityEngine;
using namespace TMPro;
namespace Java = MetaCore::Java;

bool added = false;

constexpr int EMOJI_SIZE = 108;
constexpr int SHEET_TILES = 10;
constexpr int SHEET_SIZE = SHEET_TILES * EMOJI_SIZE;

int currentEmojiIndex;
bool textureNeedsApply;
TMP_SpriteAsset* rootEmojiAsset;
TMP_SpriteAsset* currentEmojiAsset;

struct ThreadGlobals {
    jobject bitmap;
    jobject canvas;
    jobject paint;
};
std::map<std::thread::id, ThreadGlobals> jobjects = {};

std::string utf8ToInt(int unicode) {
    std::stringstream strm;
    strm << unicode;
    return strm.str();
}

std::string utf8ToHex(int unicode) {
    std::stringstream stream;
    stream << std::hex << unicode;
    return stream.str();
}

bool IsCustomAsset(TMP_SpriteAsset* asset) {
    if (!asset || !rootEmojiAsset)
        return false;
    if (rootEmojiAsset == asset)
        return true;
    for (auto customAsset : ListW<UnityW<TMP_SpriteAsset>>(rootEmojiAsset->fallbackSpriteAssets)) {
        if (customAsset.ptr() == asset)
            return true;
    }
    return false;
}

bool currentIsBold;
bool currentIsItalic;
bool currentIsUnderline;
bool currentIsStrikethrough;
constexpr int maxUnicode = 0x10ffff;

// I think 11 possible styles can fit in a uint32 given the max unicode character is 0x10ffff = 1,114,111
// maybe more if some are incompatible?
int GetStyleOffset() {
    int ret = 1;
    if (currentIsBold)
        ret += 1;
    if (currentIsItalic)
        ret += 2;
    if (currentIsUnderline)
        ret += 4;
    if (currentIsStrikethrough)
        ret += 8;
    return maxUnicode * ret;
}
// https://developer.android.com/reference/android/graphics/Typeface#constants_1
int GetTypefaceStyle() {
    int ret = 0;
    if (currentIsBold)
        ret += 1;
    if (currentIsItalic)
        ret += 2;
    return ret;
}

TMP_SpriteAsset* CreateSpriteAsset() {
    auto texture = Texture2D::New_ctor(SHEET_SIZE, SHEET_SIZE, TextureFormat::RGBA32, false);

    auto spriteAsset = ScriptableObject::CreateInstance<TMP_SpriteAsset*>();
    spriteAsset->fallbackSpriteAssets = ListW<UnityW<TMP_SpriteAsset>>::New();
    spriteAsset->spriteInfoList = ListW<TMP_Sprite*>::New();
    spriteAsset->spriteSheet = texture;

    auto mat = Resources::FindObjectsOfTypeAll<Material*>()->FirstOrDefault([](auto x) { return x->name == std::string_view("UINoGlow"); });
    spriteAsset->material = Object::Instantiate(mat);
    spriteAsset->material->mainTexture = spriteAsset->spriteSheet;

    return spriteAsset;
}

TMP_SpriteGlyph* PushSprite(int unicode) {
    if (currentEmojiIndex >= SHEET_TILES * SHEET_TILES) {
#ifdef SC2_UNICODE_APPLE_GLYPHS
        // Bound the original cache to 32 atlases (3200 styled glyphs). Exhaustion
        // returns to TMP's ordinary fallback rather than growing until OOM.
        if (rootEmojiAsset->fallbackSpriteAssets->get_Count() >= 31) return nullptr;
#endif
        auto newSheet = CreateSpriteAsset();
        rootEmojiAsset->fallbackSpriteAssets->Add(newSheet);
        currentEmojiAsset = newSheet;
        currentEmojiIndex = 0;
    }

    std::string text = utf8ToInt(unicode);

    int column = currentEmojiIndex % SHEET_TILES;
    int row = currentEmojiIndex / SHEET_TILES;

    auto glyph = TMP_SpriteGlyph::New_ctor();
    glyph->index = currentEmojiIndex;
    glyph->metrics = TextCore::GlyphMetrics(EMOJI_SIZE, EMOJI_SIZE, 0.25 * EMOJI_SIZE, 0.75 * EMOJI_SIZE, EMOJI_SIZE);
    glyph->glyphRect = TextCore::GlyphRect(column * EMOJI_SIZE, SHEET_SIZE - ((row + 1) * EMOJI_SIZE), EMOJI_SIZE, EMOJI_SIZE);

    if (!currentEmojiAsset->m_SpriteGlyphTable)
        currentEmojiAsset->m_SpriteGlyphTable = ListW<TMP_SpriteGlyph*>::New();
    currentEmojiAsset->m_SpriteGlyphTable->Add(glyph);

    TMP_SpriteCharacter* character = TMP_SpriteCharacter::New_ctor(unicode, glyph);
    character->name = text;
    character->scale = 1;

    if (!currentEmojiAsset->m_SpriteCharacterTable)
        currentEmojiAsset->m_SpriteCharacterTable = ListW<TMP_SpriteCharacter*>::New();
    currentEmojiAsset->m_SpriteCharacterTable->Add(character);

    currentEmojiAsset->SortGlyphTable();
    currentEmojiAsset->UpdateLookupTables();

    return glyph;
}

// #include "UnityEngine/Graphics.hpp"
// #include "UnityEngine/ImageConversion.hpp"
// #include "UnityEngine/Rect.hpp"
// #include "UnityEngine/RenderTexture.hpp"
// #include "UnityEngine/RenderTextureFormat.hpp"
// #include "UnityEngine/RenderTextureReadWrite.hpp"

#ifdef SC2_UNICODE_APPLE_GLYPHS
extern int sc2_unicode_draw_glyph(uint32_t,int,int,int,uint8_t*,uint32_t,uint32_t,float,float) asm("sc2_unicode_draw_glyph");
static bool RasterizeTexture(uint unicode, uint32_t* raster) {
    return !sc2_unicode_draw_glyph(unicode, GetTypefaceStyle(), currentIsUnderline,
                                   currentIsStrikethrough, reinterpret_cast<uint8_t*>(raster),
                                   EMOJI_SIZE, EMOJI_SIZE, EMOJI_SIZE * 0.75f, EMOJI_SIZE * 0.8f);
}
#endif
void DrawTexture(uint unicode, TMP_SpriteGlyph* glyph, uint32_t* rendered = nullptr) {
#ifdef SC2_UNICODE_APPLE_GLYPHS
    // Preserve the original cache, atlas and TMP hooks. A failed platform glyph
    // must fall back without inserting a blank character into the cache.
    uint32_t raster[EMOJI_SIZE * EMOJI_SIZE] = {};
    if (!rendered) { if (!RasterizeTexture(unicode, raster)) return; rendered = raster; }
    uint32_t* pixels = rendered;
#else
    auto env = Java::GetEnv();

    auto id = std::this_thread::get_id();
    if (!jobjects.contains(id))
        jobjects[id] = {};
    auto& globals = jobjects[id];

    if (!globals.bitmap) {
        logger.debug("creating bitmap for thread {}", std::hash<std::thread::id>{}(id));
        Java::JNIFrame frame(env, 8);

        auto config = Java::GetField<jobject>(env, {"android/graphics/Bitmap$Config"}, {"ARGB_8888", "Landroid/graphics/Bitmap$Config;"});
        auto tmpBitmap = Java::RunMethod<jobject>(
            env,
            {"android/graphics/Bitmap"},
            {"createBitmap", "(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;"},
            EMOJI_SIZE,
            EMOJI_SIZE,
            config
        );
        globals.bitmap = env->NewGlobalRef(tmpBitmap);
    }
    if (!globals.canvas) {
        logger.debug("creating canvas for thread {}", std::hash<std::thread::id>{}(id));
        Java::JNIFrame frame(env, 4);

        auto tmpCanvas = Java::NewObject(env, {"android/graphics/Canvas"}, "(Landroid/graphics/Bitmap;)V", globals.bitmap);
        globals.canvas = env->NewGlobalRef(tmpCanvas);
    }
    if (!globals.paint) {
        logger.debug("creating paint for thread {}", std::hash<std::thread::id>{}(id));
        Java::JNIFrame frame(env, 8);

        auto tmpPaint = Java::NewObject(env, {"android/graphics/Paint"}, "()V");
        Java::RunMethod(env, tmpPaint, {"setTextSize", "(F)V"}, (float) EMOJI_SIZE * 0.75);
        Java::RunMethod(env, tmpPaint, {"setAntiAlias", "(Z)V"}, true);
        Java::RunMethod(env, tmpPaint, {"setARGB", "(IIII)V"}, 255, 255, 255, 255);
        globals.paint = env->NewGlobalRef(tmpPaint);
    }

    Java::JNIFrame frame(env, 24);

    Java::RunMethod(env, globals.bitmap, {"eraseColor", "(I)V"}, 0);

    auto defaultTypeface = Java::GetField<jobject>(env, {"android/graphics/Typeface"}, {"DEFAULT", "Landroid/graphics/Typeface;"});
    auto typeface = Java::RunMethod<jobject>(
        env,
        {"android/graphics/Typeface"},
        {"create", "(Landroid/graphics/Typeface;I)Landroid/graphics/Typeface;"},
        defaultTypeface,
        GetTypefaceStyle()
    );

    Java::RunMethod<jobject>(env, globals.paint, {"setTypeface", "(Landroid/graphics/Typeface;)Landroid/graphics/Typeface;"}, typeface);

    Java::RunMethod(env, globals.paint, {"setUnderlineText", "(Z)V"}, currentIsUnderline);
    Java::RunMethod(env, globals.paint, {"setStrikeThruText", "(Z)V"}, currentIsStrikethrough);

    jstring str;
    int l = 1;
    if (unicode < 0xffff)
        str = env->NewString((jchar*) &unicode, 1);
    else if (unicode < maxUnicode) {
        uint n = unicode - 0x10000;
        jchar tmp[2];
        tmp[0] = 0xd800 | (n >> 10);
        tmp[1] = 0xdc00 | (n & 0x3ff);
        str = env->NewString((jchar*) tmp, 2);
        l = 2;
    } else {
        logger.warn("too big unicode {}", unicode);
        return;
    }

    Java::RunMethod(
        env,
        globals.canvas,
        {"drawText", "(Ljava/lang/String;IIFFLandroid/graphics/Paint;)V"},
        str,
        0,
        l,
        (float) 0,
        (float) EMOJI_SIZE * 0.8,
        globals.paint
    );

    // debug: saves bitmap with drawn text
    // static bool saved = false;

    // if (!saved) {
    //     auto fOut = Java::NewObject(env, {"java/io/FileOutputStream"}, "(Ljava/lang/String;)V", env->NewStringUTF("/sdcard/bitmap.png"));
    //     auto cmp = Java::GetField<jobject>(env, {"android/graphics/Bitmap$CompressFormat"}, {"PNG", "Landroid/graphics/Bitmap$CompressFormat;"});
    //     Java::RunMethod<bool>(
    //         env, globals.bitmap, {"compress", "(Landroid/graphics/Bitmap$CompressFormat;ILjava/io/OutputStream;)Z"}, cmp, 100, fOut
    //     );
    //     saved = true;
    // }

    frame.pop();

    uint32_t* pixels = nullptr;
    AndroidBitmap_lockPixels(env, globals.bitmap, (void**) &pixels);

#endif
    // ARGB8888: 4 sets of 8 bits per pixel
    int pixelsSizeU32 = EMOJI_SIZE * EMOJI_SIZE;

    // find boundaries and flip vertically
    uint32_t flippedPixels[pixelsSizeU32];
    int left = EMOJI_SIZE;
    int right = 0;
    int lower = EMOJI_SIZE;
    int upper = 0;

    for (int row = 0; row < EMOJI_SIZE; row++) {
        int rowOff = row * EMOJI_SIZE;
        int flipRowOff = EMOJI_SIZE * (EMOJI_SIZE - row - 1);
        bool foundPix = false;
        for (int col = EMOJI_SIZE - 1; col >= 0; col--) {
            auto pix = pixels[flipRowOff + col];
            if (pix != 0) {
                foundPix = true;
                if (col > right)
                    right = col;
                if (col < left)
                    left = col;
            }
            flippedPixels[rowOff + col] = pix;
        }
        if (foundPix && row < lower)
            lower = row;
        if (foundPix && row > upper)
            upper = row;
    }

#ifndef SC2_UNICODE_APPLE_GLYPHS
    AndroidBitmap_unlockPixels(env, globals.bitmap);
#endif

#ifndef SC2_UNICODE_APPLE_GLYPHS
    auto tex = Texture2D::New_ctor(EMOJI_SIZE, EMOJI_SIZE, TextureFormat::RGBA32, false);
    // tex->LoadRawTextureData(flippedPixels, pixelsSizeU32 * 4);  // size in uint8_t*
    static auto method = il2cpp_utils::FindMethodUnsafe(tex, "LoadRawTextureData", 2);
    il2cpp_utils::RunMethod<void, false>(tex, method, System::IntPtr(flippedPixels), pixelsSizeU32 * 4);
    tex->Apply();
#endif

    // debug: saves texure after copy
    // static bool saved2 = false;

    // if (!saved2) {
    //     MetaCore::Engine::WriteTexture(tex, "/sdcard/char.png");
    //     saved2 = true;
    //     logger.info("l {} r {} b {} t {}", left, right, lower, upper);
    // }

    int column = currentEmojiIndex % SHEET_TILES;
    int row = currentEmojiIndex / SHEET_TILES;
    int x = column * EMOJI_SIZE;
    int y = SHEET_SIZE - ((row + 1) * EMOJI_SIZE);

#ifdef SC2_UNICODE_APPLE_GLYPHS
    // CopyTexture_Region was stripped from the supplied Unity image. Update
    // the same original atlas through the retained managed region API instead.
    static_assert(sizeof(UnityEngine::Color32)==4);
    auto colors=Array<UnityEngine::Color32>::NewLength(pixelsSizeU32);
    std::memcpy(colors->begin(),flippedPixels,pixelsSizeU32*4);
    auto *atlas=static_cast<Texture2D*>(currentEmojiAsset->spriteSheet.ptr());
    atlas->SetPixels32(x,y,EMOJI_SIZE,EMOJI_SIZE,colors);
    atlas->Apply(false,false);
#else
    static auto CopyTexture_Region =
        il2cpp_utils::resolve_icall<void, UnityEngine::Texture*, int, int, int, int, int, int, UnityEngine::Texture*, int, int, int, int>(
            "UnityEngine.Graphics::CopyTexture_Region"
        );
    CopyTexture_Region(tex, 0, 0, 0, 0, tex->width, tex->height, currentEmojiAsset->spriteSheet, 0, 0, x, y);
#endif

    // add a little bit of spacing
    right = std::min(right + (EMOJI_SIZE / 10), EMOJI_SIZE);
    int width = right - left;
    if (width < 0)
        width = 0;
    x += left;
    int height = EMOJI_SIZE;
    // int height = upper - lower;
    // if (height < 0)
    //     height = 0;
    // y += lower;
    glyph->metrics = TextCore::GlyphMetrics(width, height, 0.125 * EMOJI_SIZE, 0.875 * EMOJI_SIZE, width);
    glyph->glyphRect = TextCore::GlyphRect(x, y, width, height);

    currentEmojiAsset->SortGlyphTable();
    currentEmojiAsset->UpdateLookupTables();
}

using namespace GlobalNamespace;

#ifdef SC2_AOT_HOOKS
struct SC2BuildHookTarget { uint64_t address; uint32_t originalHook,reserved; char name[128]; };
static_assert(sizeof(SC2BuildHookTarget)==144);
#define SC2_RECORD_HOOK(name_, mPtr) \
    static const SC2BuildHookTarget sc2_target_##name_ __attribute__((used,section(".sc2_hooks"))) = { \
        ::il2cpp_utils::il2cpp_type_check::MetadataGetter<mPtr>::addrs, 0, 0, #name_ };
#else
#define SC2_RECORD_HOOK(name_, mPtr)
#endif

SC2_RECORD_HOOK(MainFlowCoordinator_DidActivate, &MainFlowCoordinator::DidActivate)
MAKE_HOOK_MATCH(
    MainFlowCoordinator_DidActivate,
    &MainFlowCoordinator::DidActivate,
    void,
    MainFlowCoordinator* self,
    bool firstActivation,
    bool addedToHierarchy,
    bool screenSystemEnabling
) {
    if (!added) {
        rootEmojiAsset = CreateSpriteAsset();
        currentEmojiAsset = rootEmojiAsset;
        currentEmojiIndex = 0;
        TMP_Settings::get_instance()->m_defaultSpriteAsset = rootEmojiAsset;

        added = true;
    }

    MainFlowCoordinator_DidActivate(self, firstActivation, addedToHierarchy, screenSystemEnabling);
}

SC2_RECORD_HOOK(TMP_Text_GetTextElement, &TMP_Text::GetTextElement)
MAKE_HOOK_MATCH(
    TMP_Text_GetTextElement,
    &TMP_Text::GetTextElement,
    TMP_TextElement*,
    TMP_Text* self,
    uint unicode,
    TMP_FontAsset* fontAsset,
    FontStyles fontStyle,
    FontWeight fontWeight,
    ByRef<bool> isUsingAlternativeTypeface
) {
    currentIsBold = (int) fontStyle & (int) FontStyles::Bold;
    currentIsItalic = (int) fontStyle & (int) FontStyles::Italic;
    currentIsUnderline = (int) fontStyle & (int) FontStyles::Underline;
    currentIsStrikethrough = (int) fontStyle & (int) FontStyles::Strikethrough;

    return TMP_Text_GetTextElement(self, unicode, fontAsset, fontStyle, fontWeight, isUsingAlternativeTypeface);
}

SC2_RECORD_HOOK(TMP_FontAssetUtilities_GetSpriteCharacterFromSpriteAsset, &TMP_FontAssetUtilities::GetSpriteCharacterFromSpriteAsset)
MAKE_HOOK_MATCH(
    TMP_FontAssetUtilities_GetSpriteCharacterFromSpriteAsset,
    &TMP_FontAssetUtilities::GetSpriteCharacterFromSpriteAsset,
    TMP_SpriteCharacter*,
    uint unicode,
    TMP_SpriteAsset* spriteAsset,
    bool includeFallbacks
) {
    uint originalUnicode = unicode;
#ifdef SC2_UNICODE_APPLE_GLYPHS
    if (!rootEmojiAsset || !spriteAsset) return TMP_FontAssetUtilities_GetSpriteCharacterFromSpriteAsset(unicode, spriteAsset, includeFallbacks);
    if(spriteAsset==rootEmojiAsset && (unicode>0x10ffff || (unicode>=0xd800 && unicode<=0xdfff)))return nullptr;
#endif
    // don't offset multible times for the fallbacks
    if (spriteAsset == rootEmojiAsset)
        unicode += GetStyleOffset();

    auto result = TMP_FontAssetUtilities_GetSpriteCharacterFromSpriteAsset(unicode, spriteAsset, includeFallbacks);

    if (!result && spriteAsset == rootEmojiAsset) {
        logger.debug("unicode {} with style {}", originalUnicode, unicode);

#ifdef SC2_UNICODE_APPLE_GLYPHS
        uint32_t raster[EMOJI_SIZE * EMOJI_SIZE] = {};
        if (!RasterizeTexture(originalUnicode, raster)) return nullptr;
#endif
        auto glyph = PushSprite(unicode);
        if(!glyph)return nullptr;
#ifdef SC2_UNICODE_APPLE_GLYPHS
        DrawTexture(originalUnicode, glyph, raster);
#else
        DrawTexture(originalUnicode, glyph);
#endif
        currentEmojiIndex++;

        currentEmojiAsset->spriteCharacterLookupTable->TryGetValue(unicode, byref(result));

        // debug: saves whole sprite sheet
        // auto tex = (Texture2D*) currentEmojiAsset->spriteSheet.ptr();
        // MetaCore::Engine::WriteTexture(tex, "/sdcard/sprites.png");
    }
    return result;
}

SC2_RECORD_HOOK(TMP_Text_SaveSpriteVertexInfo, &TMP_Text::SaveSpriteVertexInfo)
MAKE_HOOK_MATCH(TMP_Text_SaveSpriteVertexInfo, &TMP_Text::SaveSpriteVertexInfo, void, TMP_Text* self, Color32 vertexColor) {
    // this check may not be necessary if there are no other sprite assets in the game
    std::optional<bool> tintState = std::nullopt;
    if (IsCustomAsset(self->m_textInfo->characterInfo[self->m_characterCount].spriteAsset)) {
        tintState = self->m_tintSprite;
        self->m_tintSprite = true;
    }

    TMP_Text_SaveSpriteVertexInfo(self, vertexColor);

    if (tintState.has_value())
        self->m_tintSprite = *tintState;
}

extern "C" __attribute__((visibility("default"))) void setup(CModInfo* info) {
    info->id = MOD_ID;
    info->version = VERSION;
    modInfo.assign(*info);

    Paper::Logger::RegisterFileContextId(MOD_ID);

    logger.info("Completed setup!");
}

extern "C" __attribute__((visibility("default"))) void late_load() {
    il2cpp_functions::Init();

    logger.info("Installing hooks...");
    INSTALL_HOOK(logger, MainFlowCoordinator_DidActivate);
    INSTALL_HOOK(logger, TMP_Text_GetTextElement);
    INSTALL_HOOK(logger, TMP_FontAssetUtilities_GetSpriteCharacterFromSpriteAsset);
    INSTALL_HOOK(logger, TMP_Text_SaveSpriteVertexInfo);
    logger.info("Installed all hooks!");
}

#if defined(SC2_UNICODE_APPLE_GLYPHS) && SC2_DIAGNOSTICS
#include "bsml/shared/BSML.hpp"
#include "bsml/shared/BSML/MainThreadScheduler.hpp"
extern "C" int sc2_test_unicode_raster();
extern "C" __attribute__((visibility("default"))) int sc2_test_unicode_upload_api(){
    auto *region=il2cpp_utils::FindMethodUnsafe(classof(Texture2D*),"SetPixels32",5);
    auto *apply=il2cpp_utils::FindMethodUnsafe(classof(Texture2D*),"Apply",2);
    auto *read=il2cpp_utils::FindMethodUnsafe(classof(Texture2D*),"GetPixels32",0);
    if(!region||!region->methodPointer||!apply||!apply->methodPointer||!read||!read->methodPointer)return 1;
    std::fprintf(stderr,"UNICODE_UPLOAD_API_READY retained managed SetPixels32(region), Apply and GetPixels32 methods found\n");return 0;
}
extern "C" __attribute__((visibility("default"))) int sc2_test_unicode_lifecycle(){
    if(sc2_test_unicode_raster()||sc2_test_unicode_upload_api())return 1;
    auto *texture=Texture2D::New_ctor(2,2,TextureFormat::RGBA32,false);
    auto colors=Array<UnityEngine::Color32>::NewLength(1);
    colors->_values[0].r=17;colors->_values[0].g=34;
    colors->_values[0].b=51;colors->_values[0].a=255;
    texture->SetPixels32(1,1,1,1,colors);texture->Apply(false,false);
    auto readback=texture->GetPixels32();
    auto c=readback[3];bool correct=c.r==17&&c.g==34&&c.b==51&&c.a==255;
    UnityEngine::Object::DestroyImmediate(texture);
    if(!correct)return 2;
    std::fprintf(stderr,"[scotland2] UNICODE_REGION_UPLOAD_OK actual Unity managed region write, Apply and pixel readback passed\n");
    BSML::Register::RegisterMenuButton("Unicode test: 漢 😀", "Unicode rendering check", []{});
    BSML::MainThreadScheduler::ScheduleUntil([]{return added&&rootEmojiAsset;},[]{
        for(uint32_t cp:{0x6f22u,0x1f600u}){
            auto *glyph=TMP_FontAssetUtilities::GetSpriteCharacterFromSpriteAsset(cp,rootEmojiAsset,true);
            int count=currentEmojiIndex;
            auto *again=TMP_FontAssetUtilities::GetSpriteCharacterFromSpriteAsset(cp,rootEmojiAsset,true);
            if(!glyph||!glyph->glyph||glyph->glyph->metrics.width<=0||again!=glyph||currentEmojiIndex!=count){
                std::fprintf(stderr,"[scotland2] UNICODE_GLYPH_CACHE_FAILED U+%X\n",cp);return;
            }
        }
        std::fprintf(stderr,"[scotland2] UNICODE_GLYPH_CACHE_OK original TMP fallback, texture atlas creation/upload and repeat lookup for CJK and color emoji\n");
    });return 0;
}
#endif
