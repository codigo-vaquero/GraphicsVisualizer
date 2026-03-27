#include "RepoHandling.h"
#include <fstream>
#include <filesystem> //For crearting directories and checking if they exist
#include <nlohmann/json.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem;

void RepoHandling::createFile(){
    std::string directory = "C:/ImageViewer";
    std::string path = directory + "/repositories.json";

    try{
        if(!fs::exists(directory)){
            fs::create_directories(directory);
        }

        json j;
        j["repositorios"] = json::array();

		//Write the JSON to the file
        std::ofstream file(path);
        if (file.is_open()) {
			file << j.dump(4); //Pretty print with 4 spaces indentation
            file.close();
            
            //Archivo creado exitosamente en:, path);
        }else{
            //Error: No se pudo abrir el archivo para escritura.
        }
    }catch(const std::exception& e){
        //Excepción: &e
    }
}
