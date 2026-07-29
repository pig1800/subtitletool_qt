#include <QApplication>
#include <QFont>
#include <QPalette>
#include "MainWindow.h"

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("Subtitle Editor");
    app.setFont(QFont("Segoe UI", 8));
    // Darker background via palette (preserves native checkbox/radio rendering)
    QPalette appPal = app.palette();
    appPal.setColor(QPalette::Window, QColor(0xe0, 0xe0, 0xe0));
    appPal.setColor(QPalette::Button, QColor(0xd0, 0xd0, 0xd0));
    app.setPalette(appPal);

    app.setStyleSheet(
        "QTableView { background-color: white; }"
        "QListWidget { background-color: white; }"
        "QDialog QLineEdit, QScrollArea QLineEdit { background-color: white; }"
        "QDialog QPlainTextEdit { background-color: white; }"
        "QPushButton { border: 1px solid #a0a0a0; padding: 2px 6px; }"
        "QPushButton:hover { background-color: #c8c8c8; }"
        "QPushButton:pressed { background-color: #b8b8b8; }"
    );

    MainWindow w;
    w.resize(1700, 800);
    w.setMinimumSize(900, 500);
    w.show();

    return app.exec();
}
