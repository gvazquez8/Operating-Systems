#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <ctype.h>

#include <string>
#include <deque>
#include <vector>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>

void ResetCanonicalMode(int fd, struct termios *savedattributes){
    tcsetattr(fd, TCSANOW, savedattributes);
}

void SetNonCanonicalMode(int fd, struct termios *savedattributes){
    struct termios TermAttributes;
    char *name;

    // Make sure stdin is a terminal.
    if(!isatty(fd)){
        fprintf (stderr, "Not a terminal.\n");
        exit(0);
    }

    // Save the terminal attributes so we can restore them later.
    tcgetattr(fd, savedattributes);

    // Set the funny terminal modes.
    tcgetattr (fd, &TermAttributes);
    TermAttributes.c_lflag &= ~(ICANON | ECHO); // Clear ICANON and ECHO.
    TermAttributes.c_cc[VMIN] = 1;
    TermAttributes.c_cc[VTIME] = 0;
    tcsetattr(fd, TCSAFLUSH, &TermAttributes);
}

void executeBell() {
    int bell = 0x07;
    write(STDIN_FILENO, &bell, 1);
}

void printCurrentDirectory() {
    std::string dir = std::string(get_current_dir_name());

    if (dir.length() > 16) {
        std::size_t lastDir = dir.find_last_of("/\\");
        dir = "/..." + dir.substr(lastDir) + "% ";
        write(STDIN_FILENO, dir.c_str(), dir.length());
    }
    else {
        write(STDIN_FILENO, dir.c_str(), dir.length());
        write(STDIN_FILENO, "% ", 2);
    }
}

void handlePrintCurrentDirectory() {
    std::string dir = std::string(get_current_dir_name());

    write(STDIN_FILENO, dir.c_str(), dir.length());
    write(STDIN_FILENO, "\n", 1);
}

void handleChangeDirectory(std::vector<std::string>* args) {
    if (args->size() == 1) { // no directory given, go to HOME directory
        chdir(getenv("HOME"));
    }
    else {
        std::string newDir = args->at(1);
        if (newDir == ".") {
            return;
        } else if (newDir == "/") {
            chdir("/");
        } else if (newDir == "\\") {
            chdir(getenv("HOME"));
        } else if (newDir == "..") {
            std::string currDir = std::string(get_current_dir_name());
            std::size_t pos = currDir.find_last_of("/\\");
            chdir(currDir.substr(0, pos+1).c_str());
        } else {
            std::string currDir =  std::string(get_current_dir_name());
            chdir(newDir.c_str());
            if (currDir == std::string(get_current_dir_name())) {
                std::string err = "Error: No such directory found\n";
                write(STDIN_FILENO, err.c_str(), err.length());
            }
        }
    }
    return;
}

void handleList(std::vector<std::string>* args) {
    struct dirent* pDirent;
    struct stat fileStat;
    if (args->size() == 1) {
        const char* path = get_current_dir_name();
        DIR* dir = opendir(path);
        if (dir == NULL) {
            std::string err = "Cannot open directory";
            write(STDIN_FILENO, err.c_str(), err.size());
        }
        while ((pDirent = readdir(dir)) != NULL) {
            std::string filePath = std::string(path) + "/" + pDirent->d_name;
            int good = stat(filePath.c_str(), &fileStat);
            printf( (S_ISDIR(fileStat.st_mode)) ? "d" : "-");
            printf( (fileStat.st_mode & S_IRUSR) ? "r" : "-");
            printf( (fileStat.st_mode & S_IWUSR) ? "w" : "-");
            printf( (fileStat.st_mode & S_IXUSR) ? "x" : "-");
            printf( (fileStat.st_mode & S_IRGRP) ? "r" : "-");
            printf( (fileStat.st_mode & S_IWGRP) ? "w" : "-");
            printf( (fileStat.st_mode & S_IXGRP) ? "x" : "-");
            printf( (fileStat.st_mode & S_IROTH) ? "r" : "-");
            printf( (fileStat.st_mode & S_IWOTH) ? "w" : "-");
            printf( (fileStat.st_mode & S_IXOTH) ? "x" : "-");
            printf(" %s\n", pDirent->d_name);
        }
    } else {

    }
}
std::string getNewLine(int* endProg, std::deque<std::string>* history) {
    std::string command = "";
    std::string tempCpy;
    bool searchedHistory = false;
    char currChar;
    int historyIndex = -1;

    while(1) {

        read(STDIN_FILENO, &currChar, 1);
        if (currChar == 0x04) { //C-d
            *endProg = 1;
            return "";
        } else if (currChar == 0x0A) { // Enter
            if (command != "") {
               history->push_front(command);
            }
            write(STDIN_FILENO, "\n", 1);
            return command;
        } else if (isprint(currChar)) { // If printable character
            command.push_back(currChar);
            write(STDIN_FILENO, &currChar, 1);
        } else if (currChar == 0x7F) { // Backspace
            if (command.empty()) {
                executeBell();
            } else {
                write(STDIN_FILENO, "\b", 1);
                write(STDIN_FILENO, " ", 1);
                write(STDIN_FILENO, "\b", 1);
                command.pop_back();
            }
        } else if (currChar == 0x1B) { // ECS
            read(STDIN_FILENO, &currChar, 1);
            if (currChar == 0x5B) { // [ character
                read(STDIN_FILENO, &currChar, 1);
                if (currChar == 0x41) { // A -> this is UP
                    int size = history->size();
                    if (historyIndex >= size-1) {
                        executeBell();
                    } else {
                        if (!searchedHistory) {
                            tempCpy = command;
                            searchedHistory = true;
                        }
                        historyIndex++;
                        command = history->at(historyIndex);
                        std::string clear = "\33[2K\r"; // clear line and reset
                        write(STDIN_FILENO, clear.c_str(), clear.length());
                        printCurrentDirectory();
                        write(STDIN_FILENO, command.c_str(), command.length());
                    }
                } else if (currChar == 0x42) { // B -> this is DOWN
                    if (historyIndex == -1) {
                        executeBell();
                    } else {
                        historyIndex--;
                        if (historyIndex == -1) {
                            command = tempCpy;
                            searchedHistory = false;
                            std::string clear = "\33[2K\r";
                            write(STDIN_FILENO, clear.c_str(), clear.length());
                            printCurrentDirectory();
                            write(STDIN_FILENO, command.c_str(),
                                  command.length());
                        } else {
                            std::string clear = "\33[2K\r";
                            write(STDIN_FILENO, clear.c_str(), clear.length());
                            printCurrentDirectory();
                            command = history->at(historyIndex);
                            write(STDIN_FILENO, command.c_str(),
                                  command.length());
                        }
                    }
                } else if (currChar == 0x33) {
                    read(STDIN_FILENO, &currChar, 1);
                    if (currChar == 0x7E) {
                        if (command.empty()) {
                            executeBell();
                        } else {
                            write(STDIN_FILENO, "\b", 1);
                            write(STDIN_FILENO, " ", 1);
                            write(STDIN_FILENO, "\b", 1);
                            command.pop_back();
                        }
                    }
                }
            }
        }
    }
}

void parseCommand(std::string userInput, std::vector<std::string>* args) {

    std::size_t pos = 0;

    while (!userInput.empty()) {
        pos = userInput.find(" ");
        if (pos != std::string::npos) {
            std::string partialArg = userInput.substr(0, pos);
            if (partialArg.compare("")) {
                args->push_back(partialArg);
            }
            userInput.erase(userInput.begin(), userInput.begin()+pos+1);
        } else {
            args->push_back(userInput);
            return;
        }
    }

    return;
}

void executeCommand(std::string input, int* endProg) {
    // args will be in the form of
    // [command, arg1, arg2, ...]

    std::vector<std::string> args;
    parseCommand(input, &args);

    if (args.empty()) { // if any commands were inputted
        return;
    }

    if (args[0] == "pwd") { // Print Working Directory
        handlePrintCurrentDirectory();
    } else if (args[0] == "cd") { // Change Directory
        handleChangeDirectory(&args);
    } else if (args[0] == "ls") { // List Directory
        handleList(&args);
    } else if (args[0] == "ff") {
    } else if (args[0] == "exit") { // Exit Program
        *endProg = 1;
    } else if (args[0] == "") { // Do nothing
        return;
    } else { // If command not supported, echo out command
        std::string prefix = "Command: ";
        write(STDIN_FILENO, prefix.c_str(), prefix.length());
        for (int i = 0; i < args.size(); i++) {
            write(STDIN_FILENO, args[i].c_str(), args[i].length());
            write(STDIN_FILENO, " ", 1);
        }
        write(STDIN_FILENO, "\n", 1);
    }
}

int main(int argc, char *argv[]){
    struct termios SavedTermAttributes;
    char RXChar;

    int endProg = 0; // Flag to end prog
    std::deque<std::string> history; // Command History

    SetNonCanonicalMode(STDIN_FILENO, &SavedTermAttributes);

    while(1){

        printCurrentDirectory();
        std::string userInput = getNewLine(&endProg, &history);
        executeCommand(userInput, &endProg);

        while (history.size() > 10) {
            history.pop_back();
        }

        if (endProg) { break; }
    }

    ResetCanonicalMode(STDIN_FILENO, &SavedTermAttributes);
    return 0;
}
