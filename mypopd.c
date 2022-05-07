// FOR GRADING

#include "netbuffer.h"
#include "mailuser.h"
#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>

#define MAX_LINE_LENGTH 1024
#define noop "noop"
#define quit "quit"
#define user "user"
#define pass "pass"
#define stat "stat"
#define list "list"
#define retr "retr"
#define dele "dele"
#define rset "rset"

static void handle_client(int fd);
void print(char* words);
void println(char* words);

static mail_list_t mail_list;
//static char* username_cpy;

int main(int argc, char *argv[]) {

    if (argc != 2) {
        fprintf(stderr, "Invalid arguments. Expected: %s <port>\n", argv[0]);
        return 1;
    }

    run_server(argv[1], handle_client);

    return 0;
}

void closeConnection(int error, net_buffer_t *nb, int fd) {
    send_formatted(fd, error ? "-ERR something weird happened, bye\r\n" : "+OK cya later!!\r\n");
    print("closing connection...\r\n");
    nb_destroy(*nb);
    close(fd);
}

// For debugging

void print(char* words) { fprintf(stderr, "%s", words); }

void println(char* words) { fprintf(stderr, "%s\r\n", words); }

// Helpers for parsing user commands

char* strToLower(char* word) {
    int i = 0;
    char chr;

    // word MUST be null terminated
    while (word[i]) {
        chr = word[i];
        word[i] = tolower(chr);
        i++;
    }

    return word;
}

int isNULL(char* string) { return string == NULL; }

int isNoopCommand(char* string) { return !strcmp(string, noop); }

int isQuitCommand(char* string) { return !strcmp(string, quit); }

int isUserCommand(char* string) { return !strcmp(string, user); }

int isPassCommand(char* string) { return !strcmp(string, pass); }

int isStatCommand(char* string) { return !strcmp(string, stat); }

int isListCommand(char* string) { return !strcmp(string, list); }

int isRetrCommand(char* string) { return !strcmp(string, retr); }

int isDeleCommand(char* string) { return !strcmp(string, dele); }

int isRsetCommand(char* string) { return !strcmp(string, rset); }

void parsePASSCommandLegacy(char* string, char** command, char** password) {
    int i = 0;
    char space = ' ';
    *command = string;
    while (string[i] && string[i] != space) {
        i++;
    }
    if (!string[i]) { // reached a null byte, no password given
        *password = NULL;
        return;
    }
    string[i++] = '\0';
    *password = &string[i];
}

void parsePassword(char* password, char** passParts) {
    int i = 1;
    if (passParts[i] == NULL) return;
    while(passParts[++i] != NULL);
    i--; // index of start of last part of password
    for (char* k = passParts[1]; k < passParts[i]; k++) {
        if (*k == '\0') *k = ' ';
    }
}

void handle_client(int fd) {

    char recvbuf[MAX_LINE_LENGTH + 1] = {};
    net_buffer_t nb = nb_create(fd, MAX_LINE_LENGTH);
    // int error = 0;
    int loggedIn = 0;

    send_formatted(fd, "+OK Server ready\r\n");

    /*

    The AUTHORIZATION State

    In this state, a user can type:
        - USER <username>
            - PASS <password>
        - QUIT
        - Any other commands will cause error response

    When the user successfully logs in, they will move on to the transaction state

    */

    while (!loggedIn && nb_read_line(nb, recvbuf) > 0) {

        int validCommand = 0;


        print("input: ");
        print(recvbuf);

        char* parts[6];
        split(recvbuf, parts);
        char* command = parts[0];


        // CHECK COMMAND NOT NULL
        if (isNULL(command)) {
            send_formatted(fd, "-ERR please provide at least one argument\r\n");
            // recvbuf = {};
            continue;
        }

        strToLower(command);

        // QUIT
        if (isQuitCommand(command)) {
            closeConnection(0, &nb, fd);
            return;
        }

        // NOOP
        if (isNoopCommand(command)) {
            send_formatted(fd, "+OK noop\r\n");
            continue;
        }

        // USER
        if (isUserCommand(command)) {

            if (isNULL(parts[1])) {
                send_formatted(fd, "-ERR please provide a username\r\n");
                continue;
            }

            char username[strlen(parts[1]) + 1];
            strcpy(username, parts[1]);

            print("received username: ");
            println(username);

            if (!is_valid_user(username, NULL)) {
                send_formatted(fd, "-ERR woops, invalid user! Try again?\r\n");
                continue;
            }

            send_formatted(fd, "+OK awesome!! Now maybe a password?\r\n");

            // PASS
            while(nb_read_line(nb, recvbuf) > 0 && !loggedIn) {

                char* parts[6];
                split(recvbuf, parts);
                char* command = parts[0];
                char* password = parts[1];
                parsePassword(password, parts);

                if (isNULL(command)) {
                    send_formatted(fd, "-ERR please provide at least one argument!\r\n");
                    continue;
                }

                strToLower(command);

                if (isQuitCommand(command)) {
                    closeConnection(0, &nb, fd);
                    return;
                }

                if (isPassCommand(command)) {

                    if (isNULL(password)) {
                        send_formatted(fd, "-ERR no password provided!!\r\n");
                        continue;
                    }

                    print("username: ");
                    println(username);
                    print("password: ");
                    println(password);

                    if (is_valid_user(username, password)) {
                        send_formatted(fd, "+OK success!! You are now logged in! Server has entered transaction state.\r\n");
                        loggedIn = 1;
                        validCommand = 1;

                        mail_list = load_user_mail(username);

                        break;
                    }

                    send_formatted(fd, "-ERR woops, wrong password! Try again?\r\n");
                    continue;
                }

                    // unrecognized command
                else send_formatted(fd, "-ERR unrecognized command, try >> PASS <password>!\r\n");
            }
        }

        if (!validCommand) { send_formatted(fd, "-ERR unrecognized command, maybe try >> USER <username>!\r\n"); }
    }

    if (!loggedIn) {
        closeConnection(1, &nb, fd);
        return;
    }

    /*

    The TRANSACTION State

    In this state, a user can type:
        - QUIT
        - STAT
        - NOOP
        - LIST
        - RETR
        - DELE
        - RSET
        - Any other commands will cause error response

    */

    while (nb_read_line(nb, recvbuf) > 0) {

        int validCommand = 0;

        print("input: ");
        print(recvbuf);

        char* parts[6];
        split(recvbuf, parts);
//        mail_list = load_user_mail(username_cpy);
        int total_mail_count = get_mail_count(mail_list, 1);
        int undeleted_mail_count = get_mail_count(mail_list, 0);
        int mail_size = get_mail_list_size(mail_list);

        // CHECK FIRST ARG NOT NULL
        if (parts[0] == NULL) {
            send_formatted(fd, "-ERR please provide at least one argument\r\n");
            continue;
        }

        strToLower(parts[0]);

        // NOOP
        if (!strcmp(parts[0], noop)) {
            send_formatted(fd, "+OK noop\r\n");
            continue;
        }

        // QUIT
        if (!strcmp(parts[0], quit)) {
            destroy_mail_list(mail_list);
            closeConnection(0, &nb, fd);
            return;
        }

        // STAT
        if (!strcmp(parts[0], stat)) {
            send_formatted(fd, "+OK %d %d\r\n", undeleted_mail_count, mail_size);
            continue;
        }

        // LIST
        if (!strcmp(parts[0], list)) {

            char* parameter = parts[1];

            // if no argument
            if (parameter == NULL) {

                send_formatted(fd, "+OK %d messages (%d octets)\r\n", undeleted_mail_count, mail_size);

                // List all the messages infomation
                for (int i = 0; i < total_mail_count; i++) {

                    mail_item_t item = get_mail_item(mail_list, i);

                    // print all undeleted email
                    if (item != NULL) {
                        int size = get_mail_item_size(item);
                        send_formatted(fd, "%d %d\r\n", i+1, size);
                    }

                }

                send_formatted(fd, ".\r\n");

            } // if an argument
            else {

                int pos = atoi(parameter);

                // input is not valid
                if (pos <= 0) {
                    send_formatted(fd, "-ERR not valid argument\r\n");
                    continue;
                }

                // position of the message is within the scope
                if (pos <= total_mail_count) {

                    mail_item_t item = get_mail_item(mail_list, pos - 1);

                    // print email if it exits
                    if (item != NULL) {
                        int size = get_mail_item_size(item);
                        send_formatted(fd, "+OK %d %d\r\n", pos, size);
                    } else {
                        send_formatted(fd, "-ERR message %d does not exist\r\n", pos);
                    }

                } else {
                    send_formatted(fd, "-ERR no such message, only %d messages in maildrop\r\n", undeleted_mail_count);
                }
            }

            continue;
        }

        // RETR
        if (!strcmp(parts[0], retr)) {

            char* parameter = parts[1];

            // if no argument
            if (parameter == NULL) {
                send_formatted(fd, "-ERR not valid argument\r\n");
            } // if argument
            else {

                int pos = atoi(parameter);

                // input is not valid
                if (pos <= 0) {
                    send_formatted(fd, "-ERR not valid argument\r\n");
                    continue;
                }

                // position of the message is within the scope
                if (pos <= total_mail_count) {

                    mail_item_t item = get_mail_item(mail_list, pos - 1);

                    // print contents in the email if it exists
                    if (item != NULL) {
                        int size = get_mail_item_size(item);

                        send_formatted(fd, "+OK %d octets\r\n", size);
                        FILE* file = get_mail_item_contents(item);

                        if (file != NULL) {
                            char data[MAX_LINE_LENGTH];
                            while(fgets(data, MAX_LINE_LENGTH, file) != NULL) {
                                send_formatted(fd, "%s", data);
                            }
                        }

                        fclose(file);
                        send_formatted(fd, ".\r\n"); // TODO: when content has no newline at the end? what happens to terminator?

                    } else {
                        send_formatted(fd, "-ERR message %d does not exist\r\n", pos);
                    }

                } else {
                    send_formatted(fd, "-ERR no such message, only %d messages in maildrop\r\n", undeleted_mail_count);
                }

            }

            continue;
        }

        // DELE
        if (!strcmp(parts[0], dele)) {

            char* parameter = parts[1];

            // if no argument
            if (parameter == NULL) {
                send_formatted(fd, "-ERR not valid argument\r\n");
            } // if argument
            else {

                int pos = atoi(parameter);

                // input is not valid
                if (pos <= 0) {
                    send_formatted(fd, "-ERR not valid argument\r\n");
                    continue;
                }

                // position of the message is within the scope
                if (pos <= total_mail_count) {

                    mail_item_t item = get_mail_item(mail_list, pos - 1);

                    // if item exist
                    if (item != NULL) {
                        mark_mail_item_deleted(item);
                        send_formatted(fd, "+OK message %d deleted\r\n", pos);
                    } // if item not exist
                    else {
                        send_formatted(fd, "-ERR message %d already deleted\r\n", pos);
                    }

                } else {
                    send_formatted(fd, "-ERR no such message, only %d messages in maildrop\r\n", undeleted_mail_count);
                }

            }

            continue;
        }

        // RSET
        if (!strcmp(parts[0], rset)) {
            int restored_message = reset_mail_list_deleted_flag(mail_list);
            send_formatted(fd, "+OK %d messages restored\r\n", restored_message); // PA required syntax
            continue;
        }

        if (!validCommand) { send_formatted(fd, "-ERR unrecognized command!\r\n"); }

    }

    closeConnection(1, &nb, fd);
}
