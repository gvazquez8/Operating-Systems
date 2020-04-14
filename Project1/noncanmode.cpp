#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <fcntl.h>

#include <string>
#include <deque>
#include <vector>


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

void handleRedirect(auto* args) {

    int firstPos = args->size();
    for (int i = 0; i < args->size(); i++) {
        if (args->at(i).compare(">") == 0) {
            if (i < firstPos) {
                firstPos = i;
            }
            int file_desc = open(args->at(i+1).c_str(), O_WRONLY | O_TRUNC | O_CREAT, S_IRWXU);
            if (file_desc < 0) {
                std::string err = "No such file or directory\n";
                write(STDIN_FILENO, err.c_str(), err.size());
                close(file_desc);
                exit(0);
            }
            dup2(file_desc, STDOUT_FILENO);
            dup2(file_desc, STDIN_FILENO);
            close(file_desc);

        }
        if (args->at(i).compare("<") == 0) {
            if (i < firstPos) {
                firstPos = i;
            }
            int file_desc = open(args->at(i+1).c_str(), O_RDONLY, S_IRWXU);
            if (file_desc < 0) {
                std::string err = "No such file or directory\n";
                write(STDIN_FILENO, err.c_str(), err.size());
                close(file_desc);
                exit(0);
            }
            dup2(file_desc, STDIN_FILENO);
            close(file_desc);
        }
    }

    args->erase(args->begin()+firstPos, args->end());

}

void handlePipe() {

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
        if (chdir(newDir.c_str()) < 0) {
            std::string err = "Error changing directory\n";
            write(STDIN_FILENO, err.c_str(), err.size());
        }
    }

    return;
}

void writePermissions(auto filePath) {
    struct stat fileStat;
    if (stat(filePath.c_str(), &fileStat) < 0) {
        std::string err = "File Permissions Error for ";
        write(STDIN_FILENO, err.c_str(), err.size());
    }
    else {
        write(STDIN_FILENO, (S_ISDIR(fileStat.st_mode)) ? "d" : "-", 1);
        write(STDIN_FILENO, (fileStat.st_mode & S_IRUSR) ? "r" : "-", 1);
        write(STDIN_FILENO, (fileStat.st_mode & S_IWUSR) ? "w" : "-", 1);
        write(STDIN_FILENO, (fileStat.st_mode & S_IXUSR) ? "x" : "-", 1);
        write(STDIN_FILENO, (fileStat.st_mode & S_IRGRP) ? "r" : "-", 1);
        write(STDIN_FILENO, (fileStat.st_mode & S_IWGRP) ? "w" : "-", 1);
        write(STDIN_FILENO, (fileStat.st_mode & S_IXGRP) ? "x" : "-", 1);
        write(STDIN_FILENO, (fileStat.st_mode & S_IROTH) ? "r" : "-", 1);
        write(STDIN_FILENO, (fileStat.st_mode & S_IWOTH) ? "w" : "-", 1);
        write(STDIN_FILENO, (fileStat.st_mode & S_IXOTH) ? "x" : "-", 1);
    }
}

void handleList(std::vector<std::string>* args) {
    struct dirent* pDirent;

    if (args->size() > 1) {
        std::vector<std::string> temp;
        temp.push_back("cd");
        temp.push_back(args->at(1));
        handleChangeDirectory(&temp);
    }
    const char* path = get_current_dir_name();
    DIR* dir = opendir(path);

    if (dir == NULL) {
        std::string err = "Failed to open directory \"" + std::string(path) + "\"";
        write(STDIN_FILENO, err.c_str(), err.size());
        return;
    }

    while ((pDirent = readdir(dir)) != NULL) {
        std::string filePath = std::string(path) + "/" + pDirent->d_name;
        writePermissions(filePath);
        std::string fileName = pDirent->d_name;
        fileName = " " + fileName + "\n";
        write(STDIN_FILENO, fileName.c_str(), fileName.size());
    }

    if (closedir(dir) < 0) {
        std::string err = "Failed to close directory\n";
        write(STDIN_FILENO, err.c_str(), err.size());
    }
    return;

}

void searchDirectoryForFile(std::string fileName, std::string writeBuffer) {
    DIR* dir;
    struct dirent* pDirent;
    struct stat fileStats;
    std::string path = std::string(get_current_dir_name());

    dir = opendir(path.c_str());
    if (dir == NULL) {
        std::string err = "Failed to open directory \"" + path + "\"";
        write(STDIN_FILENO, err.c_str(), err.size());
        return;
    }

    while ((pDirent = readdir(dir)) != NULL) {

        std::string filePath = path + "/" + pDirent->d_name;
        std::string currentFileName = pDirent->d_name;
        if (currentFileName == "." || currentFileName == "..") {
            continue;
        }
        if (stat(filePath.c_str(), &fileStats) < 0) {
            std::string err = "File Permissions Error for " + currentFileName + "\n";
            write(STDIN_FILENO, err.c_str(), err.size());
            continue;
        }

        if (S_ISDIR(fileStats.st_mode)) {
            // printf("%s is a directory\n", currentFileName.c_str());
            writeBuffer.append(currentFileName);
            writeBuffer.append("/");
            std::string newDir = path + "/" + currentFileName;
            chdir(newDir.c_str());
            searchDirectoryForFile(fileName, writeBuffer);
            newDir = "..";
            for (int i = 0; i < currentFileName.size()+1; i++) {
                writeBuffer.pop_back();
            }
            chdir(newDir.c_str());
        }
        else {
            if (fileName.compare(currentFileName) == 0) {
                write(STDIN_FILENO, writeBuffer.c_str(), writeBuffer.size());
                write(STDIN_FILENO, currentFileName.c_str(), currentFileName.size());
                write(STDIN_FILENO, "\n", 1);

            }
        }
    }
    if (closedir(dir) < 0) {
        std::string err = "Failed to close directory\n";
        write(STDIN_FILENO, err.c_str(), err.size());
    }
}

void handleFF(std::vector<std::string>* args) {
    if (args->size() == 1) {
        std::string err = "ff requires a filename!\n";
        write(STDIN_FILENO, err.c_str(), err.size());
        return;
    }
    if (args->size() > 2) {
        std::vector<std::string> temp;
        temp.push_back("cd");
        temp.push_back(args->at(2));
        handleChangeDirectory(&temp);
    }
    std::string writeBuffer = "./";
    searchDirectoryForFile(args->at(1), writeBuffer);
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
        pos = userInput.find("<<");

        if (pos != std::string::npos) {
            printf("EXECUTED <<\n");
            std::string space = " ";
            userInput.insert(pos+2, space.c_str());
            userInput.erase(userInput.begin() + (int)pos);
            userInput.insert(pos, space.c_str());
        }

        pos = userInput.find(">>");
        if (pos != std::string::npos) {
            std::string space = " ";
            userInput.insert(pos+2, space.c_str());
            userInput.erase(userInput.begin() + (int)pos+1);
            userInput.insert(pos, space.c_str());
        }


        pos = userInput.find_first_of("|<>");

        if (pos != std::string::npos) {
            std::string space = " ";

            if (pos == 0) {
                if (userInput[pos+1] != ' ') {
                    userInput.insert(pos+1, space.c_str());
                }
            }
            else if (pos > 0 && pos < userInput.size()) {
                if (userInput[pos+1] != ' ') {
                    userInput.insert(pos+1, space.c_str());
                }
                if (userInput[pos-1] != ' ') {
                    userInput.insert(pos, space.c_str());
                }
            }
        }


        pos = userInput.find_first_of(" ");
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

    std::vector<std::string> args;
    bool hasRedirection = false;
    bool hasPipe = false;
    parseCommand(input, &args);
    for(auto s : args) {
        if (s.find_first_of("<>") != std::string::npos) {
            hasRedirection = true;
        }
        if (s.find("|") != std::string::npos) {
            hasPipe = true;
        }
    }

    if (args.empty()) { // if any commands were inputted
        return;
    }

    // Commands for Parent Process
    if (args[0] == "cd") {
        handleChangeDirectory(&args);
        return;
    }
    else if (args[0] == "exit") {
        *endProg = 1;
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        std::string err = "Frok Failed";
        write(STDIN_FILENO, err.c_str(), err.size());
    }

    // Commands for Child Process
    if (pid == 0) {

        if (hasRedirection) {
            handleRedirect(&args);
        }

        if (hasPipe) {
            handlePipe();
        }
        if (args[0] == "pwd") { // Print Working Directory
            handlePrintCurrentDirectory();
        } else if (args[0] == "ls") { // List Directory
            handleList(&args);
        } else if (args[0] == "ff") {
            handleFF(&args);
        } else { // Use exec for command
            char* execArgs[args.size()+1];
            for(int i = 0; i < args.size(); i++) {
                execArgs[i] = (char*)args[i].c_str();
            }
            execArgs[args.size()] = NULL;

            if (execvp(execArgs[0], execArgs) < 0) {
                std::string err = "Unable to execute command\n";
                write(STDIN_FILENO, err.c_str(), err.size());
            }
        }
        exit(0);
    }
    else {
        wait(NULL);
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
