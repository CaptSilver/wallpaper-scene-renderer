#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest.h>
#include <QCoreApplication>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    doctest::Context ctx;
    ctx.applyCommandLine(argc, argv);
    return ctx.run();
}
