#include "BSML/Parsing/BSMLAction.hpp"

extern void collect_minfos(Il2CppClass* klass, std::vector<const MethodInfo*>& minfos);

namespace BSML {
    BSMLAction::BSMLAction(System::Object* host, const MethodInfo* methodInfo) : host(host), methodInfo(methodInfo) {}

    BSMLAction* BSMLAction::MakeAction(System::Object* host, std::string methodName, int argc) {
        const MethodInfo* minfo = nullptr;
        auto klass = host->klass;
        while (klass && !minfo) {
            minfo = il2cpp_functions::class_get_method_from_name(klass, methodName.c_str(), argc);
            klass = klass->parent;
        }
        
        return new BSMLAction(host, minfo);
    }

    std::map<std::string, BSMLAction*> BSMLAction::MakeActions(System::Object* host) {
        std::vector<const MethodInfo*> minfos;
        collect_minfos(host->klass, minfos);

        std::map<std::string, BSMLAction*> actions;
        for (auto minfo : minfos) {
            actions.insert({minfo->name, new BSMLAction(host, minfo)});
        }

        return actions;
    }
}
