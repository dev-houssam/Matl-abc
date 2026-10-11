/* imgui_wrapper.cpp — mini-wrapper C pour Dear ImGui (C++)
 * ImGui est "immediate mode" : chaque frame est reconstruite de
 * zero via des appels C++ (ImGui::Begin/Text/Button/End). Sans
 * backend de rendu reel (OpenGL/SDL/GLFW), ce test verifie que le
 * CONTEXTE et la CONSTRUCTION DE FRAME fonctionnent (creation de
 * contexte, ImGui::NewFrame/Text/Render/EndFrame), sans rasterisation
 * a l'ecran -- suffisant pour valider le pont FFI. */
#include "imgui.h"

extern "C" {

void* imgui_create_context() {
    IMGUI_CHECKVERSION();
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800, 600);
    /* Construire l'atlas de polices manuellement -- normalement fait
       par un backend de rendu (imgui_impl_opengl3 etc), absent ici
       puisque ce test valide le pont FFI, pas le rendu ecran. */
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsAlpha8(&pixels, &width, &height);
    return (void*)ctx;
}

void imgui_new_frame() {
    ImGui::NewFrame();
}

void imgui_begin_window(const char* title) {
    ImGui::Begin(title);
}

void imgui_text(const char* text) {
    ImGui::Text("%s", text);
}

int imgui_button(const char* label) {
    return ImGui::Button(label) ? 1 : 0;
}

void imgui_end_window() {
    ImGui::End();
}

void imgui_render() {
    ImGui::Render();
}

int imgui_get_vertex_count() {
    ImDrawData* dd = ImGui::GetDrawData();
    return dd ? dd->TotalVtxCount : -1;
}

void imgui_destroy_context(void* ctx) {
    ImGui::DestroyContext((ImGuiContext*)ctx);
}

const char* imgui_get_version() {
    return IMGUI_VERSION;
}

}
