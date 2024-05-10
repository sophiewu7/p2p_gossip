#include <iostream>
#include <cstdlib>
#include <string>

void stopAllProcesses(const std::string& processName) {
    std::string command = "pkill -f ";
    command += processName;

    int result = std::system(command.c_str());

    if (result == 0) {
        std::cout << "Successfully terminated all instances of " << processName << "." << std::endl;
    } else {
        std::cerr << "Failed to terminate instances of " << processName << ". Result code: " << result << std::endl;
    }
}

int main() {
    std::string processName = "./process";

    stopAllProcesses(processName);

    return 0;
}
