/** Standard-input entrypoint for the Sun language server. */
#include "server/server.h"

/** Runs the language server and returns its protocol shutdown status. */
int main() { return sun::lsp::runServer(); }
