#include "hooking.hpp"

#include "HMUI/ImageView.hpp"
#include "UnityEngine/UI/VertexHelper.hpp"
#include "UnityEngine/Vector2.hpp"
#include "UnityEngine/Vector3.hpp"
#include "UnityEngine/Vector4.hpp"

float lastCurvedUIRadius = 0.0f;
MAKE_AUTO_HOOK_ORIG_MATCH(ImageView_GenerateFilledSprite, &HMUI::ImageView::GenerateFilledSprite, void, HMUI::ImageView* self, UnityEngine::UI::VertexHelper* vertexHelper, bool preserveAspect, float curvedUIRadius) {
    lastCurvedUIRadius = curvedUIRadius;
    ImageView_GenerateFilledSprite(self, vertexHelper, preserveAspect, curvedUIRadius);
}

// apparently beatgames doesn't take the curved radius into account in this method so we override it with our own
MAKE_AUTO_HOOK_ORIG_MATCH(ImageView_AddQuad, static_cast<void (*)(UnityEngine::UI::VertexHelper*, ArrayW<::UnityEngine::Vector3>, UnityEngine::Color32, ArrayW<::UnityEngine::Vector3>)>(&HMUI::ImageView::AddQuad), void, UnityEngine::UI::VertexHelper* vertexHelper, ArrayW<UnityEngine::Vector3> quadPositions, UnityEngine::Color32 color, ArrayW<UnityEngine::Vector3> quadUVs) {
    int currentVertCount = vertexHelper->get_currentVertCount();
    auto uv = UnityEngine::Vector2(lastCurvedUIRadius, 0.0f);

    for (int i = 0; i < 4; i++) {
        vertexHelper->AddVert(
            quadPositions[i],
            color,
            {quadUVs[i].x, quadUVs[i].y, 0, 0},
            {0, 0, 0, 0},
            {uv.x, uv.y, 0, 0},
            {0, 0, 0, 0},
            {0, 0, 0},
            {0, 0, 0, 0}
        );
    }
    vertexHelper->AddTriangle(currentVertCount, currentVertCount + 1, currentVertCount + 2);
    vertexHelper->AddTriangle(currentVertCount + 2, currentVertCount + 3, currentVertCount);
}
