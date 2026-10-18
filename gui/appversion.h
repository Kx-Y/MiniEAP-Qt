#ifndef MINIEAP_GUI_VERSION_H
#define MINIEAP_GUI_VERSION_H

/*
 * GUI version, kept in one place so the command line, the About box and the
 * window title always agree with the engine's VERSION (minieap_common.h).
 *
 * A build system may override it with -DMINIEAP_GUI_VERSION="x.y".
 */
#ifndef MINIEAP_GUI_VERSION
#define MINIEAP_GUI_VERSION "0.93"
#endif

/* Stringify once here: QStringLiteral needs a literal, not a macro name */
#define MINIEAP_GUI_VERSION_STR MINIEAP_GUI_VERSION

#endif
