#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <ctype.h>

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

void handlepwd() {
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

        if (newDir.length() == 1) {
            // if (newDir == ".") {
            //     return;
            // } else if (newDir == "\\") {
            //     chdir(getenv("HOME"));
            // } else if (newDir == "")
        }
    }
}

void executeBell() {
    int bell = 0x07;
    write(STDIN_FILENO, &bell, 1);
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
                        std::string clear = "\33[2K\r";
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

    if (userInput == "") {
        args->push_back("");
    }

    while (!userInput.empty()) {
        pos = userInput.find(" ");
        if (pos != std::string::npos) {
            args->push_back(userInput.substr(0, pos));
            userInput.erase(0, pos+1);
        } else {
            args->push_back(userInput);
            return;
        }
    }
}
// TODO: Execute commands: cd, ls, pwd, ff, exit
// TODO: Add flags to commands
void execCommand(std::string input, int* endProg) {
    // args will be in the form of
    // [command, arg1, arg2, ...]

    std::vector<std::string> args;
    parseCommand(input, &args);

    if (args[0] == "pwd") { // Print Working Directory
        handlepwd();
    } else if (args[0] == "cd") { // Change Directory
        handleChangeDirectory(&args);
    } else if (args[0] == "exit") { // Exit Program
        *endProg = 1;
    } else if (args[0] == "") { // Do nothing
        return;
    } else { // If command not supported, echo out command
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
        execCommand(userInput, &endProg);

        while (history.size() > 10) {
            history.pop_back();
        }

        if (endProg) { break; }

        // read(STDIN_FILENO, &RXChar, 1);
        // if(0x04 == RXChar){ // C-d
        //     break;
        // }
        // else{
        //     if(isprint(RXChar)){
        //         printf("RX: '%c' 0x%02X\n",RXChar, RXChar);
        //     }
        //     else{
        //         printf("RX: ' ' 0x%02X\n",RXChar);
        //     }
        // }
    }

    ResetCanonicalMode(STDIN_FILENO, &SavedTermAttributes);
    return 0;
}
