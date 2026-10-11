/* qt_wrapper.cpp — mini-wrapper C pour Qt6 (C++)
 * Qt est du C++ pur (classes, signaux/slots bases sur moc) -- aucune
 * API C native. Ce wrapper extern "C" expose le strict minimum pour
 * un test FFI depuis MATL-ABC. */
#include <QApplication>
#include <QWidget>
#include <QLabel>

extern "C" {

static QApplication* g_app = nullptr;

void qt_initialize() {
    static int argc = 1;
    static char prog_name[] = "matlabc_qt";
    static char* argv[2];
    argv[0] = prog_name;
    argv[1] = nullptr;
    g_app = new QApplication(argc, argv);
}

void* qt_widget_new(int w, int h) {
    QWidget* widget = new QWidget();
    widget->resize(w, h);
    return (void*)widget;
}

void qt_widget_set_title(void* widget, const char* title) {
    ((QWidget*)widget)->setWindowTitle(QString::fromUtf8(title));
}

void qt_widget_show(void* widget) {
    ((QWidget*)widget)->show();
}

void qt_process_events() {
    if (g_app) g_app->processEvents();
}

void qt_widget_destroy(void* widget) {
    delete (QWidget*)widget;
}

int qt_version_major() { return QT_VERSION_MAJOR; }
int qt_version_minor() { return QT_VERSION_MINOR; }

}
