/* wx_wrapper.cpp — mini-wrapper C pour wxWidgets (C++)
 * wxWidgets n'a pas d'API C directe (classes C++) -- ce wrapper
 * extern "C" expose le strict minimum pour un test FFI depuis
 * MATL-ABC : initialiser wxWidgets sans App framework complet,
 * creer une wxFrame (fenetre), l'afficher, la detruire. */
#include <wx/wx.h>

extern "C" {

void wx_initialize() {
    int argc = 0;
    char** argv = nullptr;
    wxEntryStart(argc, argv);
}

void wx_uninitialize() {
    wxEntryCleanup();
}

void* wx_frame_new(const char* title, int w, int h) {
    wxFrame* frame = new wxFrame(nullptr, wxID_ANY, wxString(title),
                                  wxDefaultPosition, wxSize(w, h));
    return (void*)frame;
}

void wx_frame_show(void* frame) {
    ((wxFrame*)frame)->Show(true);
}

void wx_frame_destroy(void* frame) {
    ((wxFrame*)frame)->Destroy();
}

int wx_get_version_major() { return wxMAJOR_VERSION; }
int wx_get_version_minor() { return wxMINOR_VERSION; }

}
