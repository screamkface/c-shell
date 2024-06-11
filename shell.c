#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <limits.h>
#include <signal.h>
#include <errno.h>

#define MY_MAX_INPUT 1024
#define MAX_ARGS 64
#define DELIM " \t\r\n\a"
#define PIPE_DELIM "|"

#define COLOR_GREEN "\033[0;32m"
#define COLOR_RESET "\033[0m"

char lastdir[1024] = ""; // Variabile globale per tenere traccia dell'ultima directory
char curdir[1024];       // Variabile globale per la directory corrente

void print_green(const char *str)
{
    printf("\033[0;32m%s\033[0m", str);
}

void prompt()
{
    getcwd(curdir, sizeof(curdir));
    char *username = getlogin();
    char hostname[_SC_HOST_NAME_MAX];
    gethostname(hostname, sizeof(hostname));
    printf("%s%s@%s%s:%s$ ", COLOR_GREEN, username, hostname, COLOR_RESET, curdir);
}

char *read_line()
{
    char *line = NULL;
    size_t bufsize = 0;
    getline(&line, &bufsize, stdin);
    return line;
}

char **split_line(char *line, const char *delim)
{
    int bufsize = MAX_ARGS, position = 0;
    char **tokens = malloc(bufsize * sizeof(char *));
    char *token;

    if (!tokens)
    {
        fprintf(stderr, "shell: allocation error\n");
        exit(EXIT_FAILURE);
    }

    token = strtok(line, delim);
    while (token != NULL)
    {
        tokens[position] = token;
        position++;

        if (position >= bufsize)
        {
            bufsize += MAX_ARGS;
            tokens = realloc(tokens, bufsize * sizeof(char *));
            if (!tokens)
            {
                fprintf(stderr, "shell: allocation error\n");
                exit(EXIT_FAILURE);
            }
        }

        token = strtok(NULL, delim);
    }
    tokens[position] = NULL;
    return tokens;
}

// Gestore per il segnale SIGTSTP
void sigtstp_handler(int sig)
{
    printf("\nCaught signal %d (SIGTSTP). Use 'exit' to quit the shell.\n", sig);
    prompt();
    fflush(stdout);
}

int cd(char **args)
{
    char *dir = args[1];
    if (dir == NULL)
    {
        dir = getenv("HOME");
    }
    else if (strcmp(dir, "-") == 0)
    {
        if (lastdir[0] == '\0')
        {
            fprintf(stderr, "shell: no previous directory\n");
            return 1;
        }
        dir = lastdir;
    }
    else if (dir[0] == '~')
    {
        char path[1024];
        snprintf(path, sizeof(path), "%s%s", getenv("HOME"), dir + 1);
        dir = path;
    }

    // Aggiorna la directory corrente
    if (chdir(dir) != 0)
    {
        perror("shell");
        return 1;
    }

    // Aggiorna lastdir
    strcpy(lastdir, curdir);
    getcwd(curdir, sizeof(curdir));
    return 0;
}

int execute(char **args)
{
    // Verifica se il comando contiene una pipe
    int i;
    for (i = 0; args[i] != NULL; i++)
    {
        if (strcmp(args[i], PIPE_DELIM) == 0)
        {
            break; // Trovata la pipe
        }
    }

    if (args[i] == NULL)
    {
        // Nessuna pipe trovata, verifica se il comando è "cd" o "exit"
        if (strcmp(args[0], "cd") == 0)
        {
            return cd(args);
        }
        else if (strcmp(args[0], "exit") == 0)
        {
            return 0; // esce dal loop
        }

        // Esegui il comando normalmente
        pid_t pid;
        int status;

        pid = fork();
        if (pid == 0)
        {
            // Processo figlio
            if (execvp(args[0], args) == -1)
            {
                perror("shell");
            }
            exit(EXIT_FAILURE);
        }
        else if (pid < 0)
        {
            // Errore nella fork
            perror("shell");
        }
        else
        {
            // Processo padre
            do
            {
                waitpid(pid, &status, WUNTRACED);
            } while (!WIFEXITED(status) && !WIFSIGNALED(status));
        }
    }
    else
    {
        // Pipe trovata, esegui i comandi prima e dopo la pipe
        char **cmd1 = args;         // Comando prima della pipe
        char **cmd2 = &args[i + 1]; // Comando dopo la pipe
        args[i] = NULL;             // Termina il primo comando con NULL

        int pipefd[2];
        pid_t pid1, pid2;
        int status1, status2;

        if (pipe(pipefd) == -1)
        {
            perror("pipe");
            return 1;
        }

        pid1 = fork();

        if (pid1 == 0) // siamo nel processo figlio 1
        {
            close(pipefd[0]); // chiudiamo la pipe in lettura
            dup2(pipefd[1], STDOUT_FILENO);
            close(pipefd[1]);

            // eseguiamo il primo comando
            if (execvp(cmd1[0], cmd1) == -1)
            {
                perror("shell");
            }
            exit(EXIT_FAILURE);
        }
        else if (pid1 < 0)
        {
            // errore nella fork
            perror("fork");
            return 1;
        }

        pid2 = fork();
        if (pid2 == 0)
        {
            close(pipefd[1]); // chiudiamo in scrittura la pipe
            dup2(pipefd[0], STDIN_FILENO);
            close(pipefd[0]); // chiudiamo in lettura la pipe

            // eseguiamo il secondo comando
            if (execvp(cmd2[0], cmd2) == -1)
            {
                perror("shell");
            }
            exit(EXIT_FAILURE);
        }
        else if (pid2 < 0)
        {
            perror("fork");
            return 1;
        }

        // Chiudi entrambe le estremità della pipe nel processo padre
        close(pipefd[0]);
        close(pipefd[1]);

        // Attendi che entrambi i processi figlio terminino
        waitpid(pid1, &status1, 0);
        waitpid(pid2, &status2, 0);
    }

    return 1; // Ritorna sempre 1 per continuare il ciclo
}

int main(int argc, char const *argv[])
{
    char *line;
    char **args;
    int status = 1;

    // Imposta il gestore di segnali per SIGTSTP
    signal(SIGTSTP, sigtstp_handler);

    do
    {
        if (isatty(STDIN_FILENO))
        {
            // Shell in modalità interattiva
            prompt();
        }

        line = read_line();
        args = split_line(line, DELIM);
        if (args[0] != NULL) // Verifica se è stato inserito un comando
        {
            status = execute(args);
        }

        free(line);
        free(args);

    } while (status);

    return 0;
}