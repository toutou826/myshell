#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>

void processCommand(int currentCommand);
void restoreRedirect();
void terminateChildren();
void signalHandler(int signno, siginfo_t* info, void* vp);
void removeSlashN(char *line);
int readCommand(char *inputbuf, char *commands[50], char *separators[50]);
void executeCommand(char *argumentsbuffer, int isBackground);
void doPipe(char *argumentsBuffer, int isBackground);


char *commands[50];
char *separators[50]; 
int numCommands;
int currentCommand;
int originalIn;
int originalOut;
int originalErr;
pid_t shellpgid;
pid_t bgLeader = -1;


// get actual commands seperated by ; and |, 
// record those in commands and record the separators(| or ;) in separators
// return the total of actual commands we have to run
int readCommand(char *inputbuf, char *commands[50], char *separators[50]) {

  int numCommands = 0; 
  char *saveptr1, *saveptr2;

  char *tokenPipe;
  //split and parse '|'
  //get a token
  tokenPipe = strtok_r(inputbuf, "|", &saveptr1);

  while (tokenPipe) {
    //split and parse ; inside
    if (strstr(tokenPipe, ";")) {
      char *tokenComma;
      tokenComma = strtok_r(tokenPipe, ";", &saveptr2);
      while (tokenComma) {
        strcpy(commands[numCommands++], tokenComma);
        strcpy(separators[numCommands-1], ";");
        //grab the next token
        tokenComma = strtok_r(NULL, ";", &saveptr2);
      }
      // change the last separater ;
      separators[numCommands-1] = "|";
      //grab next token after the pipe
      tokenPipe = strtok_r(NULL, "|", &saveptr1);
      continue;
    }
    strcpy(commands[numCommands++], tokenPipe);
    strcpy(separators[numCommands-1], "|");
    // grab the next token
    tokenPipe = strtok_r(NULL, "|", &saveptr1);
  }
  //remove last |
  separators[numCommands-1] = '\0';

  return numCommands;
}

// process the command, set up the redirections

void processCommand(int currentCommand) {

  char *command = commands[currentCommand];
  char argumentsBuffer[200];
  memset(argumentsBuffer, 0, 200); 
  //whether the current program should run in background
  int isBackground = 0;

  int filedescriptor;
  char *token;

  //get a token
  token = strtok(command, " ");
  while (token) {

      if (strstr(token, "\t") != NULL){ //in case the tokens are tab seperated instead of space seperated then chop up the token
        token = strtok(token, "\t");
      }

      // if this command needs to be run in the background
      if (token[strlen(token)-1] == '&' && strstr(token, "&>") == 0) {
        //remove the & for now so it can be processed
        token[strlen(token)-1] = '\0';
        //record that
        isBackground = 1;
      }

      // regular command
      if ((strstr(token, ">") == 0) 
          && (strstr(token, "<") == 0) 
          && (strstr(token, "1>") == 0) 
          && (strstr(token, "2>") == 0) 
          && (strstr(token, "&") == 0) 
          && (strstr(token, "&>") == 0)) {
        strcat(argumentsBuffer, token);
        strcat(argumentsBuffer, " ");
      }

      if (strstr(token, ">") != 0) {
        
        char * file;
        //next token must be the file name
        file = strtok(NULL, " ");

        // open takes care of fullpath or relative path
        // O_CREAT would create the file if it is not found, or just give the fd if it is found
        filedescriptor = open(file, O_RDWR | O_TRUNC| O_CREAT, S_IRWXU);

        // if fd == -1
        if (filedescriptor < 0) {
          // problem opening the file
          fprintf(stderr, "%s: problem opening the file. \n", file);
          terminateChildren();
          exit(-1);
        }

        // determine whether it is 1> or 2> or &> or just >
        // then redirect accordingly
        switch(token[0]) {
          case '2':
            dup2(filedescriptor, 2); // have the output file replace stderr
            break;
          case '&':
            dup2(filedescriptor, 2); // have the output file replace stderr
            dup2(filedescriptor, 1); // and stdout
            break;
          case '1':
          default:
            dup2(filedescriptor, 1); // have the output file replace stdout
            break;
        }
        // close the fd when done
        close(filedescriptor);
        // at this point we have finish processing this command
        break;
      }

      if (strstr(token, "<") != 0) {

        //next token must be the file name
        token = strtok(NULL, " ");
        
        // did not include O_CREAT here because we want the fd to be -1 when file not found
        filedescriptor = open(token, O_RDWR | O_APPEND);

        if (filedescriptor < 0) {
          //if it doesn't exist then throw an error
          fprintf(stderr, "%s: No such file or directory. \n", token);
          terminateChildren();
          exit(-1);
        }

        dup2(filedescriptor, 0); // have the output file replace stdin
        close(filedescriptor);  // close the fd whe done
        //at this point we have finish processing this command
        break;
      }

      //done with this token, grab the next one
      token = strtok(NULL, " ");
  }

  // end the argument buffer because when we did the checking if is a regular command
  // we put and space after each token
  // so at the end there would be one extra space, just need to remvoe that
  argumentsBuffer[strlen(argumentsBuffer)-1] = '\0';

  doPipe(argumentsBuffer, isBackground);
}

// recursivly do pipe
void doPipe(char *argumentsBuffer, int isBackground) {

  // base case
  // if last command no no more pipe, execute directly, else do pipe
  if (currentCommand == numCommands - 1 || strcmp(separators[currentCommand], "|") != 0) {
      // use the argument buffer to excute command
      executeCommand(argumentsBuffer, isBackground);
      return;
  } else {
    // pipefd[0]: read endpoint, pipefd[1]: write endpoint
    int pipefd[2];  
    pid_t cpid;

    if (pipe(pipefd) == -1) {
       printf("An error occured creating the pipe\n");
       terminateChildren();
    }

    cpid = fork();
    char *buf;
    if (cpid == 0) {
      // CHILD CODE
      close(pipefd[1]); //close write
      // use what has been piped as the input
      dup2(pipefd[0], 0);

      close(pipefd[0]); //close read
      // process the next command
      currentCommand++;
      processCommand(currentCommand);

    } else if (cpid > 0) {
      // PARENT CODE
      close(pipefd[0]); //close read
      //redirect output to pipe
      dup2(pipefd[1], 1);

      // can execute command now that output is redirected
      executeCommand(argumentsBuffer, isBackground);
      close(pipefd[1]); // close write

      // block the shell untill the foreground process finish
      int wstatus;
      do {
        waitpid(cpid, &wstatus, 0);
      }
      while (!WIFEXITED(wstatus) && !WIFSIGNALED(wstatus));


    } else {
      printf("An error occured forking a new process\n");
      terminateChildren();
      exit(-1);
    }
    
  }

}

void removeSlashN(char *line) {
  if (line[strlen(line)-1] == '\n') {
    line[strlen(line)-1] = '\0';
  }
}

void signalHandler(int signno, siginfo_t* info, void* vp) {
  // when the parent recieves control c it ignores it
  // the foreground children will die though
  if (signno == SIGINT) {
    // kill foreground processes with the same group id as fgLeader
    pid_t fg_p = tcgetpgrp(originalIn);
    if (fg_p != shellpgid) {
      kill(SIGKILL, fg_p);
      // wait for it to die
      int wstatus;
      wait(&wstatus);
    }
  }

  if (signno == SIGCHLD) {
    // int pid = info->si_pid;
    // int gpid = getpgid(pid);
    // if (bgLeader != -1 && gpid == bgLeader) {
      // printf("\nProcess: %d just finished running.\n", pid);
    // }
  }
}

//terminate all child in background before exiting
void terminateChildren() {
  // kill all background process
  if (bgLeader != -1) {
    kill(SIGKILL, -bgLeader);
  }
  int wstatus;
  wait(&wstatus);
}

//restore the original redirects
void restoreRedirect() {
  dup2(originalIn, 0);  
  dup2(originalOut, 1);
  dup2(originalErr, 2);
}

void executeCommand(char *argumentsbuffer, int isBackground) {

  // construct argument array
  char *args[20];
  int numArgs = 0;
  char *token;

  token = strtok(argumentsbuffer, " ");
  while (token) {
    args[numArgs++] = token;
    token = strtok(NULL, " ");
  }

  // Seal arg array with a null
  args[numArgs] = NULL;


  // fork a child and execute the command
  pid_t pid = fork();


  if (pid < 0) {
    printf("Error forking the process\n");
    terminateChildren();
    exit(1);
  } else if (pid == 0) {
    // child code

    pid_t currentPid = getpid();
    // getpgid(0)
    if (isBackground) {
      // give it a group id, which makes it have a different group id than the one that controls terminal
      // which stops its access to stdin, stdout and stderr
      if (bgLeader == -1) {
        bgLeader = currentPid;
      }
      setpgid(currentPid,bgLeader);
    }
    
    // execute command
    int err = execvp(args[0], args);
    if (err == -1) {
      printf("errno: %d", errno);
    }

  } else {
    //parent code
    if (!isBackground) {
      // block the shell untill the foreground process finish
      int wstatus;
      do {
          waitpid(pid, &wstatus, 0);
      }
      while (!WIFEXITED(wstatus) && !WIFSIGNALED(wstatus));
    } else {
      // background process, don't have to wait
      // printf("process: %d running in the background.\n", pid);

      // did it in child and parent because there maybe race condition
      if (bgLeader == -1) {
        bgLeader = pid;
      }
      setpgid(pid, bgLeader);
    }
  }
  // return;
  // restore redirect to what we had when terminal started
  restoreRedirect();
}



int main(int argc, char *argv[], char *envp[])
{
  //set up sigaction structure
  struct sigaction sa;
  sa.sa_flags = SA_RESTART | SA_SIGINFO;
  sa.sa_sigaction = signalHandler;

  //register signal handler
  if (sigaction(SIGINT, &sa, NULL) < 0) {
    printf("sigaction error\n");
    terminateChildren();
    exit(1);
  }

  //register signal handler
  if (sigaction(SIGCHLD, &sa, NULL) < 0) {
    printf("sigaction error\n");
    terminateChildren();
    exit(1);
  }

  // keep track of the original fileno
  originalIn = dup(STDIN_FILENO);
  originalOut = dup(STDOUT_FILENO);
  originalErr = dup(STDERR_FILENO);

  //record the pid pid for the terminal, set the group id to itself
  shellpgid = getpid();
  setpgid(shellpgid, shellpgid);
  // tcsetpgrp(originalIn, getpid());

  while (1) {
    //means it's from a terminal so we need to print 'myshell > '
    if (isatty(STDIN_FILENO) == 1) {
      printf("myshell> ");
    } 

    // get the current line
    char inputbuf[500] = "";

    if(fgets(inputbuf, 500, stdin) == NULL) {
      // if null that means we have and EOF, end myshell program
      // make sure all children are terminated too
      printf("\n");
      terminateChildren();
      exit(0);
    }

    removeSlashN(inputbuf);


    // allocate space for command and separator array
    int i;
    for (i = 0; i < 20; i++) {
      commands[i] = malloc(20 * sizeof(char));
      separators[i] = malloc(20 * sizeof(char));
    }
    
    //seperate commands by | and ;
    numCommands = readCommand(inputbuf, commands, separators);


    currentCommand = 0;
    // process and and execute command
    while (currentCommand < numCommands) {
      // process and execute command
      processCommand(currentCommand);
      // restore back to the original fileno we stored in the beginning of the program
      restoreRedirect();
      // proceed to the next
      currentCommand++;
    }
  }
}