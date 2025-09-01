#include "systemcalls.h"
// Includes needed for implementation of ToDos
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>      // system()
#include <sys/types.h>
#include <sys/wait.h>    // WIFEXITED, WEXITSTATUS, waitpid()
#include <unistd.h>      // fork(), execv(), _exit(), dup2()
#include <fcntl.h>       // open()
#include <errno.h>
#include <string.h>

/**
 * @param cmd the command to execute with system()
 * @return true if the command in @param cmd was executed
 *   successfully using the system() call, false if an error occurred,
 *   either in invocation of the system() call, or if a non-zero return
 *   value was returned by the command issued in @param cmd.
*/
bool do_system(const char *cmd)
{

    if (cmd == NULL) return false;

    int status = system(cmd);              // runs via /bin/sh -c ...
    if (status == -1) return false;        // system() failed

    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/**
* @param count -The numbers of variables passed to the function. The variables are command to execute.
*   followed by arguments to pass to the command
*   Since exec() does not perform path expansion, the command to execute needs
*   to be an absolute path.
* @param ... - A list of 1 or more arguments after the @param count argument.
*   The first is always the full path to the command to execute with execv()
*   The remaining arguments are a list of arguments to pass to the command in execv()
* @return true if the command @param ... with arguments @param arguments were executed successfully
*   using the execv() call, false if an error occurred, either in invocation of the
*   fork, waitpid, or execv() command, or if a non-zero return value was returned
*   by the command issued in @param arguments with the specified arguments.
*/

bool do_exec(int count, ...)
{
    va_list args;
    va_start(args, count);
    char * command[count+1];
    int i;
    for(i=0; i<count; i++)
    {
        command[i] = va_arg(args, char *);
    }
    
    // Build argv[] was already done above:
    // for (int i = 0; i < count; i++) command[i] = va_arg(args, char *);

    command[count] = NULL;                  // execv requires NULL-terminated argv

    pid_t pid = fork();
    if (pid < 0) {                          // fork failed
        va_end(args);
        return false;
    }

    if (pid == 0) {                         // child
        execv(command[0], command);         // replaces child on success
        _exit(1);                           // only reached if execv fails
    }

    int status = 0;                         // parent
    if (waitpid(pid, &status, 0) < 0) {
        va_end(args);
        return false;
    }

    va_end(args);
    return WIFEXITED(status) && (WEXITSTATUS(status) == 0);
}

/**
* @param outputfile - The full path to the file to write with command output.
*   This file will be closed at completion of the function call.
* All other parameters, see do_exec above
*/
bool do_exec_redirect(const char *outputfile, int count, ...)
{
    va_list args;
    va_start(args, count);

    char *command[count + 1];
    for (int i = 0; i < count; i++) {
        command[i] = va_arg(args, char *);
        if (!command[i]) { va_end(args); return false; }
    }
    command[count] = NULL;  // required by execv

    if (!outputfile) { va_end(args); return false; }

    pid_t pid = fork();
    if (pid < 0) {                     // fork failed
        va_end(args);
        return false;
    }

    if (pid == 0) {                    // child
        int fd = open(outputfile, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd < 0) _exit(1);

        if (dup2(fd, STDOUT_FILENO) < 0) _exit(1);
        if (dup2(fd, STDERR_FILENO) < 0) _exit(1);
        close(fd);

        execv(command[0], command);    // replace child; only returns on error
        _exit(1);
    }

    int status = 0;                    // parent
    if (waitpid(pid, &status, 0) < 0) {
        va_end(args);
        return false;
    }

    va_end(args);
    return WIFEXITED(status) && (WEXITSTATUS(status) == 0);
}
