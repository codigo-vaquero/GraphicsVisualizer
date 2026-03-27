#pragma once
#include <iostream>


class Repository{
	int id;
	std::string name;

public:
	Repository(int id, const std::string& name) : id(id), name(name) {}
};

