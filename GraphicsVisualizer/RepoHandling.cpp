#include "RepoHandling.h"

#include <fstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

void RepoHandling::createFile(const std::string& path){
	std::ofstream file(path);
}
