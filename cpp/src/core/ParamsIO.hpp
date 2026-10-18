#pragma once
#include <string>
#include "Params.hpp"

// JSON parameter files, compatible with the files written by the HTML version
// ("Dosyaya Kaydet (.json)" → seismic_parameters.json). Keys are the HTML element ids,
// values are strings exactly like input.value in the browser.

std::string paramsToJson(const SimParams& p, double shipX);
// Returns false (and an error message) on malformed JSON. Unknown keys are ignored.
bool paramsFromJson(const std::string& text, SimParams& p, double& shipX, std::string& err);

// Replacement for the browser's localStorage: %APPDATA%\SeismicFDTD\params.json
std::string localParamsPath();

bool readTextFile(const std::string& path, std::string& out);
bool writeTextFile(const std::string& path, const std::string& text);
