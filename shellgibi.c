#include <unistd.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>            //termios, TCSANOW, ECHO, ICANON
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>

const char *sysname = "shellgibi";
char *PATH;
char *USER;
const int MAX_MATCHES_AUTOCOMPLETE = 40;
#define NUMUSERMETHODS 6
char* userMethods[NUMUSERMETHODS] = {"alarm", "myjobs", "mybg", "myfg", "pause", "motivate"};

char **getListOfMatchingCommands();

char *onematch(char *cmd);


enum return_codes {
    SUCCESS = 0,
    EXIT = 1,
    UNKNOWN = 2,
};
struct command_t {
    char *name;
    bool background;
    bool auto_complete;
    int arg_count;
    char **args;
    char *redirects[3]; // in/out redirection
    struct command_t *next; // for piping
};

/**
 * Prints a command struct
 * @param struct command_t *
 */
void print_command(struct command_t *command) {
    int i = 0;
    printf("Command: <%s>\n", command->name);
    printf("\tIs Background: %s\n", command->background ? "yes" : "no");
    printf("\tNeeds Auto-complete: %s\n", command->auto_complete ? "yes" : "no");
    printf("\tRedirects:\n");
    for (i = 0; i < 3; i++)
        printf("\t\t%d: %s\n", i, command->redirects[i] ? command->redirects[i] : "N/A");
    printf("\tArguments (%d):\n", command->arg_count);
    for (i = 0; i < command->arg_count; ++i)
        printf("\t\tArg %d: %s\n", i, command->args[i]);
    if (command->next) {
        printf("\tPiped to:\n");
        print_command(command->next);
    }


}

/**
 * Release allocated memory of a command
 * @param  command [description]
 * @return         [description]
 */
int free_command(struct command_t *command) {
    if (command->arg_count) {
        for (int i = 0; i < command->arg_count; ++i)
            free(command->args[i]);
        free(command->args);
    }
    for (int i = 0; i < 3; ++i)
        if (command->redirects[i])
            free(command->redirects[i]);
    if (command->next) {
        free_command(command->next);
        command->next = NULL;
    }
    free(command->name);
    free(command);
    return 0;
}

/**
 * Show the command prompt
 * @return [description]
 */
int show_prompt() {
    char cwd[1024], hostname[1024];
    gethostname(hostname, sizeof(hostname));
    getcwd(cwd, sizeof(cwd));
    printf("%s@%s:%s %s$ ", getenv("USER"), hostname, cwd, sysname);
    return 0;
}

/**
 * Parse a command string into a command struct
 * @param  buf     [description]
 * @param  command [description]
 * @return         0
 */
int parse_command(char *buf, struct command_t *command) {
    const char *splitters = " \t"; // split at whitespace
    int index, len;
    len = strlen(buf);
    while (len > 0 && strchr(splitters, buf[0]) != NULL) // trim left whitespace
    {
        buf++;
        len--;
    }
    while (len > 0 && strchr(splitters, buf[len - 1]) != NULL)
        buf[--len] = 0; // trim right whitespace

    if (len > 0 && buf[len - 1] == '?') // auto-complete
        command->auto_complete = true;
    if (len > 0 && buf[len - 1] == '&') // background
        command->background = true;

    char *pch = strtok(buf, splitters);
    command->name = (char *) malloc(strlen(pch) + 1);
    if (pch == NULL)
        command->name[0] = 0;
    else
        strcpy(command->name, pch);

    command->args = (char **) malloc(sizeof(char *));

    int redirect_index;
    int arg_index = 0;
    char temp_buf[1024], *arg;
    while (1) {
        // tokenize input on splitters
        pch = strtok(NULL, splitters);
        if (!pch) break;
        arg = temp_buf;
        strcpy(arg, pch);
        len = strlen(arg);

        if (len == 0) continue; // empty arg, go for next
        while (len > 0 && strchr(splitters, arg[0]) != NULL) // trim left whitespace
        {
            arg++;
            len--;
        }
        while (len > 0 && strchr(splitters, arg[len - 1]) != NULL) arg[--len] = 0; // trim right whitespace
        if (len == 0) continue; // empty arg, go for next

        // piping to another command
        if (strcmp(arg, "|") == 0) {
            struct command_t *c = malloc(sizeof(struct command_t));
            int l = strlen(pch);
            pch[l] = splitters[0]; // restore strtok termination
            index = 1;
            while (pch[index] == ' ' || pch[index] == '\t') index++; // skip whitespaces

            parse_command(pch + index, c);
            pch[l] = 0; // put back strtok termination
            command->next = c;
            continue;
        }

        // background process
        if (strcmp(arg, "&") == 0)
            continue; // handled before

        // handle input redirection
        redirect_index = -1;
        if (arg[0] == '<')
            redirect_index = 0;
        if (arg[0] == '>') {
            if (len > 1 && arg[1] == '>') {
                redirect_index = 2;
                arg++;
                len--;
            } else redirect_index = 1;
        }
        if (redirect_index != -1) {
            command->redirects[redirect_index] = malloc(len);
            strcpy(command->redirects[redirect_index], arg + 1);
            continue;
        }

        // normal arguments
        if (len > 2 && ((arg[0] == '"' && arg[len - 1] == '"')
                        || (arg[0] == '\'' && arg[len - 1] == '\''))) // quote wrapped arg
        {
            arg[--len] = 0;
            arg++;
        }
        command->args = (char **) realloc(command->args, sizeof(char *) * (arg_index + 1));
        command->args[arg_index] = (char *) malloc(len + 1);
        strcpy(command->args[arg_index++], arg);
    }
    command->arg_count = arg_index;
    return 0;
}

void prompt_backspace() {
    putchar(8); // go back 1
    putchar(' '); // write empty over
    putchar(8); // go back 1 again
}

/**
 * Prompt a command from the user
 * @param  buf      [description]
 * @param  buf_size [description]
 * @return          [description]
 */
int prompt(struct command_t *command) {
    int index = 0;
    char c;
    static char buf[4096];
    static char oldbuf[4096];

    // tcgetattr gets the parameters of the current terminal
    // STDIN_FILENO will tell tcgetattr that it should write the settings
    // of stdin to oldt
    static struct termios backup_termios, new_termios;
    tcgetattr(STDIN_FILENO, &backup_termios);
    new_termios = backup_termios;
    // ICANON normally takes care that one line at a time will be processed
    // that means it will return if it sees a "\n" or an EOF or an EOL
    new_termios.c_lflag &= ~(ICANON | ECHO); // Also disable automatic echo. We manually echo each char.
    // Those new settings will be set to STDIN
    // TCSANOW tells tcsetattr to change attributes immediately.
    tcsetattr(STDIN_FILENO, TCSANOW, &new_termios);


    //FIXME: backspace is applied before printing chars
    show_prompt();
    int multicode_state = 0;
    buf[0] = 0;
    //printf("\nin prompt, buf is %s\n", buf);
    while (1) {
        //printf("\nwaiting to get char\n");
        c = getchar();
        // printf("Keycode: %u\n", c); // DEBUG: uncomment for debugging
        //printf("\ngot a char, buf is %s\n", buf);
        //printf("got a char, ind is %d\n", index);


        if (c == 9) // handle tab
        {
            // checking if there's only one matching command
            char *curComm = malloc(index + 1);
            for (int i = 0; i < index; i++) {
                curComm[i] = buf[i];
            }
            curComm[index] = 0;
            char *match = onematch(curComm);
            if (match != NULL) {
                //printf("got a singular match!\n");
                for (int i = index; i < strlen(match); i++) {
                    putchar(match[i]);
                    buf[index++] = match[i];
                }
            } else {
                buf[index++] = '?'; // autocomplete
                break;
            }
        }

        if (c == 127) // handle backspace
        {
            if (index > 0) {
                prompt_backspace();
                index--;
            }
            continue;
        }
        if (c == 27 && multicode_state == 0) // handle multi-code keys
        {
            multicode_state = 1;
            continue;
        }
        if (c == 91 && multicode_state == 1) {
            multicode_state = 2;
            continue;
        }
        if (c == 65 && multicode_state == 2) // up arrow
        {
            int i;
            while (index > 0) {
                prompt_backspace();
                index--;
            }
            for (i = 0; oldbuf[i]; ++i) {
                putchar(oldbuf[i]);
                buf[i] = oldbuf[i];
            }
            index = i;
            continue;
        } else
            multicode_state = 0;
        if (c != 9) { // if not tab
            putchar(c); // echo the character
            buf[index++] = c;
        }
        if (index >= sizeof(buf) - 1) break;
        if (c == '\n') // enter key
            break;
        if (c == 4) // Ctrl+D
            return EXIT;
    }
    if (index > 0 && buf[index - 1] == '\n') // trim newline from the end
        index--;
    buf[index++] = 0; // null terminate string

    strcpy(oldbuf, buf);

    parse_command(buf, command);

    //print_command(command); // DEBUG: uncomment for debugging

    // restore the old settings
    tcsetattr(STDIN_FILENO, TCSANOW, &backup_termios);
    return SUCCESS;
}

int process_command(struct command_t *command);

int process_command2(struct command_t *command, int pipe);

int main() {
    PATH = getenv("PATH");
    USER = getenv("USER");

    while (1) {
        struct command_t *command = malloc(sizeof(struct command_t));
        memset(command, 0, sizeof(struct command_t)); // set all bytes to 0

        int code;
        code = prompt(command);
        if (code == EXIT) break;

        code = process_command2(command, STDIN_FILENO);
        if (code == EXIT) break;

        free_command(command);
    }

    printf("\n");
    return 0;
}

void setArgsForExecv(struct command_t *command);

static void redirect(int oldfd, int newfd);

void printArray(char **a) {
    printf("\n");
    for (int i = 0; i < MAX_MATCHES_AUTOCOMPLETE; i++) {
        if (a[i] == NULL) {
            printf("\nprinted all matches\n");
            break;
        }
        printf("%s\n", a[i]);
    }
}

// for alarm
char *concat(const char *s1, const char *s2) {
    char *result = malloc(strlen(s1) + strlen(s2) + 1); // +1 for the null-terminator
    // in real code you would check for errors in malloc here
    strcpy(result, s1);
    strcat(result, s2);
    return result;
}



// for todo

int getCurrentLineNumber(char* filename) {
    int x;
    FILE *fp;
    char tmp[1024];
    fp = fopen(filename, "ab+");

    while (!feof(fp))
        fgets(tmp, 1024, fp);

    x = atoi(tmp);
    return x;
}

bool StartsWith(const char *a, const char *b) {
    if (strncmp(a, b, strlen(b)) == 0) return 1;
    return 0;
}

void deleteLineFromFile(char* filename, char *lineno) {
    int lno, ctr = 0;

    FILE *fptr1, *fptr2;

    char str[256], temp[] = "temp.txt";

    fptr1 = fopen(filename, "r");
    if (!fptr1) {
        printf(" File not found or unable to open the input file!!\n");

    }
    fptr2 = fopen(temp, "w"); // open the temporary file in write mode
    if (!fptr2) {
        printf("Unable to open a temporary file to write!!\n");
        fclose(fptr1);

    }

    while (!feof(fptr1)) {
        strcpy(str, "\0");
        fgets(str, 256, fptr1);
        if (!feof(fptr1)) {


            if (!StartsWith(str, lineno)) {
                fprintf(fptr2, "%s", str);
            }
        }
    }
    fclose(fptr1);
    fclose(fptr2);
    remove(filename);
    rename(temp, filename);

}

void printRandomline(char * filename) {
    FILE *fp = fopen(filename, "ab+");
    int N=0;
    char str[150];

    while (1) {
        if (fgets(str, 150, fp) == NULL) break;
        N++;
    }
    fseek(fp, 0, SEEK_SET); // reset fp
    printf("there are %d lines", N);
    if (N==0) {
        printf("empty motivational file!\n");
        return;
    }
    int randomNumber = rand() % N;
    printf("random is %d\n", randomNumber);
    int line = 0;
    while (1) {
        if (fgets(str, 150, fp) == NULL) break;
        if (line == randomNumber) {
            printf("%s\n", str);
            return;
        }
        line++;
    }
}

int process_command2(struct command_t *command, int in_fd) {
    int r;

    if (strcmp(command->name, "") == 0) return SUCCESS;

    if (strcmp(command->name, "exit") == 0)
        return EXIT;

    if (strcmp(command->name, "cd") == 0) {
        if (command->arg_count > 0) {
            r = chdir(command->args[0]);
            if (r == -1)
                printf("-%s: %s: %s\n", sysname, command->name, strerror(errno));
            return SUCCESS;
        }
    }

    // user defined commands
    if (strcmp(command->name, "alarm") == 0) {
        if (command->arg_count != 2) {
            printf("Not in the right format. Should be like the following: alarm time(hour.minute) soundFile\n");
            return SUCCESS;
        }

        char *time_arr[2];
        int c = 0;
        char s[strlen(command->args[0])];
        strcpy(s, command->args[0]);
        char *token = strtok(s, ".");
        while (token) {
            time_arr[c] = token;
            c++;
            token = strtok(NULL, ".");
        }

        // get pwd
        char pwd[1024];
        getcwd(pwd, sizeof(pwd));

        pid_t pid = fork();
        if (pid == 0) // child
        {
            char *str = "* * * aplay ";
            char *new_str = concat(str, pwd);
            char *str1 = concat(new_str, "/");
            char *str2 = concat(str1, command->args[1]);


            char *echo_args[] = {"/bin/echo", time_arr[1], time_arr[0], str2, NULL};

            int fd = open("mycron", O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
            dup2(fd, 1);
            close(fd);

            execv(echo_args[0], echo_args);
            exit(0);
        }

        wait(0);
        pid_t pid1 = fork();
        if (pid1 == 0) {

            char *cron_args[] = {"/usr/bin/crontab", "mycron", NULL};
            execv(cron_args[0], cron_args);

            exit(0);
        }
        wait(0);
        pid_t pid2 = fork();
        if (pid2 == 0) {

            char *rm_args[] = {"/bin/rm", "mycron", NULL};
            execv(rm_args[0], rm_args);

            exit(0);
        }

        wait(0);
        printf("alarm set.\n");
        return SUCCESS;
    }

    if (strcmp(command->name, "todo") == 0) {
        printf("in todo\n");
        //print_command(command);

        if (command->arg_count == 0) {
            printf("add to add a todo, see to see the todo list and del to remove from list\n");
            return SUCCESS;
        }
        if (strcmp(command->args[0], "delete") == 0) {
            deleteLineFromFile(".todo", command->args[1]);
            printf("deleted todo successfully\n");
            return SUCCESS;
        }


        if (strcmp(command->args[0], "see") == 0) {
            FILE *fptr1;
            char ch;
            fptr1 = fopen(".todo", "ab+");
            ch = fgetc(fptr1);
            while (ch != EOF) {
                printf("%c", ch);
                ch = fgetc(fptr1);
            }
            fclose(fptr1);
            return SUCCESS;
        }
        if (strcmp(command->args[0], "add") == 0) {

            int x = getCurrentLineNumber(".todo");


            //conver x+1 to string again
            int new_x = x + 1;
            char new_x_str[50];
            sprintf(new_x_str, "%d", new_x);


            pid_t pid1;
            pid1 = fork();
            if (pid1 == 0) {

                int fd = open(".todo", O_RDWR | O_CREAT | O_APPEND, S_IRUSR | S_IWUSR);
                dup2(fd, 1);
                close(fd);

                char *final_str = "";

                for (int i = 1; i < command->arg_count; ++i) {
                    final_str = concat(final_str, " ");
                    final_str = concat(final_str, command->args[i]);

                }

                char *echo_args[] = {"/bin/echo", concat("", concat(new_x_str, concat(" ", final_str))), NULL};

                execv(echo_args[0], echo_args);
                exit(0);
            }
            wait(0);
            return SUCCESS;
        }

        return SUCCESS;
    }

    // job management
    if (strcmp(command->name, "myjobs") == 0) {
        pid_t pid = fork();
        if (pid == 0) {
            char *args[6];
            args[0] = "/bin/ps";        // first arg is the full path to the executable
            args[1] = "-U";
            args[2] = USER;
            args[3] = "-eo";
            args[4] = "pid,cmd,stat";
            args[5] = NULL;
            //printf("im here\n");
            execv(args[0], args);
            exit(0);
        } else {
            wait(0);
            //printf("yay, look at me\n");
            return SUCCESS;
        }
    }

    if (strcmp(command->name, "pause") == 0) {
        // check if args match etc

        pid_t pid = fork();
        if (pid == 0) {
            char *args[4];
            args[0] = "/bin/kill";        // first arg is the full path to the executable
            args[1] = "-TSTP";
            args[2] = command->args[0];
            args[3] = NULL;
            execv(args[0], args);
            exit(0);
        } else {
            wait(0);
            return SUCCESS;
        }
    }

    if (strcmp(command->name, "mybg") == 0) {
        // check if args match etc

        pid_t pid = fork();
        if (pid == 0) {
            char *args[4];
            args[0] = "/bin/kill";        // first arg is the full path to the executable
            args[1] = "-CONT";
            args[2] = command->args[0];
            args[3] = NULL;
            printf("running cont\n");
            execv(args[0], args);
            printf("just ran cont\n");
            exit(0);
        } else {
            printf("parent\n");
            wait(0);
            printf("child done\n");

            return SUCCESS;
        }
    }
    if (strcmp(command->name, "myfg") == 0) {
        // check if args match etc

        pid_t pid = fork();
        if (pid == 0) {
            char *args[4];
            args[0] = "/bin/kill";        // first arg is the full path to the executable
            args[1] = "-CONT";
            args[2] = command->args[0];
            args[3] = NULL;
            execv(args[0], args);
            exit(0);
        } else {
            int a = atoi(command->args[0]);
            //printf("waiting for pid to finish\n");
            waitpid(a, 0, 0);
            //printf("wait over\n");

            return SUCCESS;
        }
    }

    if (strcmp(command->name, "motivate") == 0) {
        //printf("in motivate\n");
        //print_command(command);

        if (command->arg_count == 0) {
            printRandomline(".motivate");
            return SUCCESS;
        }
        else if (strcmp(command->args[0], "delete") == 0) {
            deleteLineFromFile(".motivate", command->args[1]);
            printf("deleted from motivate successfully\n");
            return SUCCESS;
        }


        else if (strcmp(command->args[0], "add") == 0) {

            int x = getCurrentLineNumber(".motivate");

            //conver x+1 to string again
            int new_x = x + 1;
            char new_x_str[50];
            sprintf(new_x_str, "%d", new_x);

            pid_t pid1;
            pid1 = fork();
            if (pid1 == 0) {

                int fd = open(".motivate", O_RDWR | O_CREAT | O_APPEND, S_IRUSR | S_IWUSR);
                dup2(fd, 1);
                close(fd);

                char *final_str = "";

                for (int i = 1; i < command->arg_count; ++i) {
                    final_str = concat(final_str, " ");
                    final_str = concat(final_str, command->args[i]);

                }

                char *echo_args[] = {"/bin/echo", concat("", concat(new_x_str, concat(" ", final_str))), NULL};

                execv(echo_args[0], echo_args);
                exit(0);
            }
            wait(0);
            return SUCCESS;
        }
    }

    pid_t pid = fork();
    if (pid == 0) // child
    {
        /// This shows how to do exec with environ (but is not available on MacOs)
        // extern char** environ; // environment variables
        // execvpe(command->name, command->args, environ); // exec+args+path+environ

        /// This shows how to do exec with auto-path resolve
        // add a NULL argument to the end of args, and the name to the beginning
        // as required by exec

        if (command->auto_complete) {
            // the last character is ?, we don't want that

            char *cmdName = malloc(strlen(command->name));
            for (int i = 0; i < strlen(command->name) - 1; i++) {
                cmdName[i] = command->name[i];
                //printf("autocomplete, %s\n", cmdName);
            }
            cmdName[strlen(command->name)] = 0;
            char **matches = getListOfMatchingCommands(cmdName);
            if (matches[0] == NULL) {
                printf("\nNo matches!\n");
            } else {
                for (int i = 0; i < MAX_MATCHES_AUTOCOMPLETE; i++) { // command fully typed
                    if (matches[i] == NULL)
                        break;
                    if (strcmp(matches[i], cmdName) == 0) {
                        printf("\n");
                        char *args[2];

                        args[0] = "/bin/ls";        // first arg is the full path to the executable
                        args[1] = NULL;
                        execv(args[0], args);
                        free(matches);
                        exit(0);
                    }
                }
                printf("\nmatching commands\n"); // print matching commands
                for (int i = 0; i < MAX_MATCHES_AUTOCOMPLETE; i++) {
                    if (matches[i] == NULL)
                        exit(0);
                    printf("%s\n", matches[i]);
                }
            }
            free(matches);
        }

        // increase args size by 2
        command->args = (char **) realloc(
                command->args, sizeof(char *) * (command->arg_count += 2));

        // shift everything forward by 1
        for (int i = command->arg_count - 2; i > 0; --i)
            command->args[i] = command->args[i - 1];


        setArgsForExecv(command);

        //execvp(command->name, command->args); // exec+args+path
        if (command->redirects[0] != NULL) {
            int fd = open(command->redirects[0], O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);

            dup2(fd, 0);

            close(fd);     // fd no longer needed - the dup'ed handles are sufficient
        }

        if (command->redirects[1] != NULL) {
            int fd = open(command->redirects[1], O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);

            dup2(fd, 1);   // make stdout go to file

            close(fd);     // fd no longer needed - the dup'ed handles are sufficient
        }

        if (command->redirects[2] != NULL) {
            int fd = open(command->redirects[2], O_RDWR | O_CREAT | O_APPEND, S_IRUSR | S_IWUSR);

            dup2(fd, 1);   // make stdout go to file

            close(fd);     // fd no longer needed - the dup'ed handles are sufficient
        }


        if (command->next != NULL) {  // Piping
            // pipe stuff
            int fd[2];
            pipe(fd);
            int pid = fork();
            if (pid == 0) { //
                close(fd[0]); /* unused */
                redirect(in_fd, STDIN_FILENO);  /* read from in_fd */
                redirect(fd[1], STDOUT_FILENO); /* write to fd[1] */

                execv(command->args[0], command->args);
                exit(0);
                //close(pfd[1]);

            } else {
                close(fd[1]); /* unused */
                close(in_fd); /* unused */
                wait(NULL);
                process_command2(command->next, fd[0]);
            }
        } else {
            redirect(in_fd, STDIN_FILENO);
            execv(command->args[0], command->args);
        }

        exit(0);

        /// TODO: do your own exec with path resolving using execv()
    } else {
        if (!command->background)
            wait(0); // wait for child process to finish
        /*if (command->auto_complete) {
            printf("\nauto\n");
        }*/
        return SUCCESS;
    }

    // TODO: your implementation here

    printf("-%s: %s: command not found\n", sysname, command->name);
    return UNKNOWN;
}


/** move oldfd to newfd */
static void redirect(int oldfd, int newfd) {
    if (oldfd != newfd)
        if (dup2(oldfd, newfd) != -1)
            close(oldfd); /* successfully redirected */
}


void setArgsForExecv(struct command_t *command) {
    char *pathCopy = malloc(strlen(PATH));
    strcpy(pathCopy, PATH);
    char *token = strtok(pathCopy, ":");


    // loop through the string to extract all other tokens
    while (token != NULL) {
        //printf( " %s\n", token ); //printing each token
        char *dir = malloc(strlen(token) + 1 + strlen(command->name));
        strcpy(dir, token);
        strcat(dir, "/");
        strcat(dir, command->name);
        //printf( " %s\n", dir );
        if (access(dir, F_OK) != -1) {
            command->args[0] = dir;
            break;
        }
        token = strtok(NULL, ":");

    }

    // set args[arg_count-1] (last) to NULL
    command->args[command->arg_count - 1] = NULL;

}

bool prefix(const char *pre, const char *str);

char *onematch(char *cmd) {
    //printf("\ngetting onematch of %s\n", cmd);
    int numMatch = 0;
    char *pathCopy = malloc(strlen(PATH));
    strcpy(pathCopy, PATH);
    char *token = strtok(pathCopy, ":");
    char *match = malloc(40);

    while (token != NULL) {
        char *dir = malloc(strlen(token));
        strcpy(dir, token);
        struct dirent *de;  // Pointer for directory entry

        // opendir() returns a pointer of DIR type.
        DIR *dr = opendir(dir);

        if (dr == NULL)  // opendir returns NULL if couldn't open directory
        {
            token = strtok(NULL, ":");
            continue;
        }

        // Refer http://pubs.opengroup.org/onlinepubs/7990989775/xsh/readdir.html
        // for readdir()
        while ((de = readdir(dr)) != NULL) {
            //printf("\nin dir\n");

            if (prefix(cmd, de->d_name)) {
                numMatch++;
                strcpy(match, de->d_name);
                /*printf("got a match!\n");
                printf("match is %s\n", match);*/
                if (numMatch > 1)
                    return NULL;
            }
        }

        closedir(dr);
        token = strtok(NULL, ":");

    }

    for (int i=0; i<NUMUSERMETHODS; i++){
        if (prefix(cmd, userMethods[i])) {
            //printf("got a match! %s\n", de->d_name);
            strcpy(match, userMethods[i]);
            numMatch++;
        }
    }
    if (numMatch == 1) {
        //printf("\ngot one match, %s\n", match);
        return match;
    }
    return NULL;
}

char **getListOfMatchingCommands(char *cmd) {
    //printf("\nGetting list of matching commands\n");
    //printf("checking for equality %s.\n", cmd);
    char *pathCopy = malloc(strlen(PATH));
    strcpy(pathCopy, PATH);
    char *token = strtok(pathCopy, ":");
    int matchCount = 0;
    int maxlen = 36;
    char **matches = malloc(MAX_MATCHES_AUTOCOMPLETE * sizeof(char *));
    for (int i = 0; i < MAX_MATCHES_AUTOCOMPLETE; ++i)
        matches[i] = malloc(maxlen * sizeof(char));


    // check for user defined methods

    for (int i=0; i<NUMUSERMETHODS; i++){
        if (prefix(cmd, userMethods[i])) {
            //printf("got a match! %s\n", de->d_name);
            strcpy(matches[matchCount++], userMethods[i]);
        }
    }

    while (token != NULL) {
        char *dir = malloc(strlen(token));
        strcpy(dir, token);
        //printf("dir is %s\n", dir);
        struct dirent *de;  // Pointer for directory entry

        // opendir() returns a pointer of DIR type.
        DIR *dr = opendir(dir);

        if (dr == NULL)  // opendir returns NULL if couldn't open directory
        {
            token = strtok(NULL, ":");
            continue;
        }

        // Refer http://pubs.opengroup.org/onlinepubs/7990989775/xsh/readdir.html
        // for readdir()
        while ((de = readdir(dr)) != NULL) {
            //printf("\nin dir\n");

            if (prefix(cmd, de->d_name)) {
                //printf("got a match! %s\n", de->d_name);
                strcpy(matches[matchCount++], de->d_name);
                //printf("Match in array: %s\n", matches[matchCount-1]);
                if (matchCount == MAX_MATCHES_AUTOCOMPLETE - 1) {
                    matches[matchCount] = NULL;
                    return matches;
                }
            }
        }

        closedir(dr);
        token = strtok(NULL, ":");

    }
    matches[matchCount] = NULL;
    //printf("\nPrinting array in method\n");
    //printArray(matches);
    return matches;

}


bool prefix(const char *pre, const char *str) {
    return strncmp(pre, str, strlen(pre)) == 0;
}