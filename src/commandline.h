#include "utils.h"

// CLI HELPERS //

// Determines if a command line invocation is for the relocate form
// of this executable
bool IsRelocate(const char* arg);

// Parses the command line for an invocation of the relocate command
// and returns the arguments mapped from argument name to value
std::map<std::string, std::string> ParseRelocate(const char** args, int argc);

// Writes CLI help message to stdout
bool CheckAndPrintHelp(const char** arg, bool no_args, bool is_relocate);