/*
 * Console entry point of MiniEAP.
 *
 * The engine itself (argument parsing, capture loop, console output) lives in
 * minieap.c and is shared with the GUI. This file only provides main() for the
 * standalone client, so the console build keeps behaving exactly like the
 * original single-file program.
 */
#include "engine.h"
#include "logging.h"
#include "minieap_common.h"
#include "minieap_runtime.h"

int main(int argc, char* argv[]) {
    if (IS_FAIL(minieap_engine_init(argc, argv, 1))) {
        return FAILURE;
    }
    return minieap_engine_run();
}
