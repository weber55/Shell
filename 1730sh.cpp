#include <cstdlib>
#include <iostream>
#include <string>
#include <cstring>
#include <sstream>
#include <vector>
#include <pwd.h>
#include <limits.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <fstream>

using namespace std;

bool inputR = false;
bool outputR = false;
bool errorR = false;
bool isTrunc = false;
bool isApp = false;
string inputStr, outputStr, errorStr;
string pwd; // present working directory
const char *home; // home directory
vector<int> jobs; // holds JIDs 
int shellpid; // PID of the parent program

// function prototypes
string prompt();
void execute(vector<string> argv);
void closepipe(int pipefd[2]);
bool isBuiltin(vector<string> strargs);
void sighandler(int signo);

struct process{
  vector<string> arguments;
  int pid;
  int status;
};

/**
 * Entry point to the program. Prompts user and executes
 * inputed commands
 */
int main(){
  cout.setf(std::ios::unitbuf);
  
  // shell should ignore job control signals
  //signal(SIGINT, SIG_IGN);
  signal(SIGQUIT, SIG_IGN);
  signal(SIGTSTP, SIG_IGN);
  signal(SIGTTIN, SIG_IGN);
  signal(SIGTTOU, SIG_IGN);
  
  // set pid of the shell program
  shellpid = getpid();

  // get home directory and set as pwd  
  if ((home = getenv("HOME")) == NULL) {
    home = getpwuid(getuid())->pw_dir;
  }
  pwd = home;
  chdir(home);

  // begin loop
  while(true){
    cout << prompt();

    // put all user input in vector
    string uinput;
    getline(cin, uinput);
    stringstream ss;
    ss.str(uinput);
    string arg;
    vector<string> args;
    while (ss >> arg){
      args.push_back(arg);
    } 
    if (args.size() < 1){
      continue;
    }

    string stdin = "STDIN_FILENO";
    string stdout = "STDOUT_FILENO";
    string stderr = "STDERR_FILENO";
    int npipes = 0;
    
    // parse user input
    vector<vector<string>> argv; // vector of process names + args
    vector<string> process;
    string quotearg;
    for (unsigned int n = 0; n < args.size(); n++){
      if (args[n].compare("<") == 0){
	inputR = true;
	inputStr = args[n+1];
	stdin = args[n+1];
	n++;
      }
      else if (args[n].compare(">") == 0){
	outputR = true;
	outputStr = args[n+1];
	isTrunc = true;
	stdout = args[n+1] + " (truncate)";
	n++;
      }
      else if (args[n].compare(">>") == 0){
	outputR = true;
        outputStr = args[n+1];
	isApp = true;
	stdout = args[n+1] + " (append)";
	n++;
      }
      else if (args[n].compare("e>") == 0){
	errorR = true;
	errorStr = args[n+1];
	isTrunc = true;
	stderr = args[n+1] + " (truncate)";
	n++;
      }
      else if (args[n].compare("e>>") == 0){
	errorR = true;
	errorStr = args[n+1];
	isApp = true;
	stderr = args[n+1] + " (append)";
	n++;
      }
      else if (args[n].compare("|") == 0){
	npipes++;
	argv.push_back(process);
	process.clear();
      }
      else { 
	// parse arguments surrounded by quotes
	if (args[n].substr(0,1).compare("\"") == 0){
	  quotearg = args[n].substr(1, args[n].size()-1);
	  n++;
	  while (n < args.size()){
	    quotearg = quotearg + " " + args[n];
	    n++;
	    if (args[n].substr(args[n].size()-1,1).compare("\"") == 0){
	      quotearg = quotearg + " " + args[n].substr(0, args[n].size()-1);
	      string b = "\\\"";
	      string a = "\"";
	      std::string::size_type x = 0;
	      while ((x = quotearg.find(b,x)) != std::string::npos){
		quotearg.replace(x, b.size(), a);
		x += a.size();
	      }
	      process.push_back(quotearg);
	      quotearg = "";
	      break;
	    }
	  }
	}
	// just add to vector if no quotes
	else {
	  process.push_back(args[n]);
	}
      } // else
    } // for
    argv.push_back(process);
    const char * inputChar = inputStr.c_str();
    const char * outputChar = outputStr.c_str();
    const char * errorChar = errorStr.c_str();
    int out_file;
    int in_file;
 
    
    int (*pipes)[2] = new int[npipes][2]; // array with pipefd[2] for each pipe
    pid_t pid, wpid;

    // true for job ending with &, indicating background job
    bool isBackground = (argv[argv.size()-1][argv[argv.size()-1].size()-1].compare("&") == 0);

    // PROCESS LOOP
    for (unsigned int n = 0; n < argv.size(); n++){ // loop through the processes
      
      if (!isBuiltin(argv[n])){ // if is not a builtin
	
	//create pipe
	if (pipe(pipes[n]) == -1){
	  perror("pipe"); 
	}
	
	// create child
	if ((pid = fork()) == -1){
	  perror("fork");
	} 
	else if (pid == 0){
	     
	  if (isBackground){
	    argv[n].pop_back(); // remove ampersand from args
	  }

	  if ((n == 0)&&(inputR)){ // If input is redirected
	    in_file = open(inputChar, O_CREAT | O_RDONLY , S_IREAD | S_IWRITE);
	    dup2(in_file, STDIN_FILENO);
	    close(in_file);
	    inputR = false;
	  }
	  if (n < argv.size()-1){ // not last 
	    if (dup2(pipes[n][1], STDOUT_FILENO) == -1){ 
	      perror("dup2 1");
	    }
	    closepipe(pipes[n]);
	  } 
	  if (n > 0){ // not first
	    if (dup2(pipes[n-1][0], STDIN_FILENO) == -1){
	      perror("dup2 2");
	    }
	    closepipe(pipes[n-1]);
	  }
	  if ((n == argv.size()-1)&&(outputR)){ // if output redirection
	    if (isTrunc){
	      out_file = open(outputChar, O_WRONLY | O_TRUNC | O_CREAT | O_CLOEXEC, S_IRUSR | S_IRGRP | S_IWGRP | S_IWUSR);
	      isTrunc = false;
	    }
	    else if (isApp){
	      out_file = open(outputChar, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, S_IRUSR | S_IRGRP | S_IWGRP | S_IWUSR);
	      isApp = false;
	    }
	    dup2(out_file, STDOUT_FILENO);
	    close(out_file);
	    outputR = false;
	  }
	  if ((n == argv.size()-1)&&(errorR)){ // if error redirection
	    if (isTrunc){
	      out_file = open(errorChar, O_CREAT | O_WRONLY | O_TRUNC, S_IRUSR | S_IRGRP | S_IWGRP | S_IWUSR);
	      isTrunc = false;
	    }
	    else if (isApp){
	      out_file = open(errorChar, O_CREAT | O_WRONLY | O_APPEND, S_IRUSR | S_IRGRP | S_IWGRP | S_IWUSR);
	      isApp = false;
	    }
	    dup2(out_file, STDERR_FILENO);
	    close(out_file);
	    errorR = false;
	  }

	  execute(argv[n]); // execute program  
	}

	// PID of first child = PGID = JID
	if (n == 0){
	  jobs.push_back(pid); 
	}
	// set process group id of each child
	if (setpgid(pid, jobs.back()) < 0){
	  perror("setpgid");
	}

	// allow process group to have access to terminal 
	if (tcsetpgrp(STDIN_FILENO, jobs.back()) < 0){
	  perror("tcsetpgrp");
	}

	// after last process is created, close pipes and wait
	if (n == argv.size()-1){
	  for (int i = 0; i < npipes; i++){
	    closepipe(pipes[i]);
	  }
	  if (!isBackground){ // if foreground job, wait for status change to reprompt
	    if ((wpid = waitpid(pid, nullptr, 0)) == -1){
	      perror("waitpid");
	    }
	    else{
	      // return terminal access to the shell
	      if (tcsetpgrp(STDIN_FILENO, shellpid) < 0){
		perror("tcsetpgrp");
	      }
	    }
	  }
	  delete [] pipes; // deallocate mems
	}

      } // if !isBuiltin
      
    } // process loop
    // wait for status changes of any jobs, display message if any
    for (unsigned int i = 0; i < jobs.size(); i++){
      int status;
      if ((jobs[i] = waitpid(WAIT_ANY, &status, WNOHANG | WUNTRACED | WCONTINUED)) == 0){
	
	if(WIFEXITED(status)){
	  cout << getpgid(jobs[i]) << " Exited" << endl;
	  if (tcsetpgrp(STDIN_FILENO, shellpid) < 0){
	    perror("tcsetpgrp");
	  }
	}
	else if(WIFSIGNALED(status)){
	  cout << getpgid(jobs[i]) << " Terminated by signal" << endl;
	  if (tcsetpgrp(STDIN_FILENO, shellpid) < 0){
	    perror("tcsetpgrp");
	  }
	}
	else if(WIFSTOPPED(status)){
	  cout << getpgid(jobs[i]) << " Stopped" << endl;
	  if (tcsetpgrp(STDIN_FILENO, shellpid) < 0){
	    perror("tcsetpgrp");
	  }
	}
	else if(WIFCONTINUED(status)){
	  cout << getpgid(jobs[i]) << " Continued" << endl;
	  if (tcsetpgrp(STDIN_FILENO, jobs[i]) < 0){
	    perror("tcsetpgrp");
	  }
	}
      }
    }

    
  } // 1730sh loop
  return EXIT_SUCCESS;
} // main


/**
 * Signal hander (not used yet)
 */
void sighandler(int signo){
  //int pid, status;
  switch(signo){

  } // switch
}

/**
 * Executes programs that are not built in
 */
void execute(vector<string> strings){ // takes in array with cmd line arg for a program

  // convert string vector to null terminating char* vector
  vector<char*> argv(strings.size() + 1); 
  for (size_t i = 0; i < strings.size(); i++){
    argv[i] = &strings[i][0];
  } 

  if (execvp(argv[0], argv.data()) < 0){
    perror("execvp");
    exit(EXIT_FAILURE);
  }
} // execute


/**
 * Checks if the entered command is a builtin. If so, executes the builtin
 */
bool isBuiltin(vector<string> strings){
  vector<char*> argv(strings.size() + 1); // convert string vector to char*                   
  for (size_t i = 0; i < strings.size(); i++){
    argv[i] = &strings[i][0];
  }

  // if built ins
  string process = argv[0];
  if (process.compare("cd") == 0){ //default to home if no args
    if(argv[1] == NULL){
      chdir(home);
    } 
    else {
      chdir(argv[1]);
    }
    char *cwd;
    char buffer[PATH_MAX + 1];
    cwd = getcwd(buffer, PATH_MAX+1);
    if (cwd != NULL){
      pwd = cwd;
    }
    return true;
  }
  else if (process.compare("jobs") == 0){
    for (unsigned int i = 0; i < jobs.size(); i++){
      int status;
      if ((jobs[i] = waitpid(WAIT_ANY, &status, WNOHANG | WUNTRACED | WCONTINUED)) == 0){

        if(WIFEXITED(status)){
          cout << getpgid(jobs[i]) << " Exited" << endl;
          if (tcsetpgrp(STDIN_FILENO, shellpid) < 0){
            perror("tcsetpgrp");
          }
        }
        else if(WIFSIGNALED(status)){
          cout << getpgid(jobs[i]) << " Terminated by signal" << endl;
          if (tcsetpgrp(STDIN_FILENO, shellpid) < 0){
            perror("tcsetpgrp");
          }
        }
        else if(WIFSTOPPED(status)){
          cout << getpgid(jobs[i]) << " Stopped" << endl;
          if (tcsetpgrp(STDIN_FILENO, shellpid) < 0){
            perror("tcsetpgrp");
          }
        }
        else if(WIFCONTINUED(status)){
          cout << getpgid(jobs[i]) << " Continued" << endl;
          if (tcsetpgrp(STDIN_FILENO, jobs[i]) < 0){
            perror("tcsetpgrp");
          }
        }
      }
    }
  }
  else if (process.compare("bg") == 0){
    for (unsigned int i = 0; i < jobs.size(); i++){
      int jid = atoi(argv[1]);
      if (jid == jobs[i]){
	// resume jobs[i] in bg
      }
      else {
	cout << "job JID: " << jid << " could not be found in background" << endl;
      }
    }
  }
  else if (process.compare("fg") == 0){
    for (unsigned int i = 0; i < jobs.size(); i++){
      int jid = atoi(argv[1]);
      if (jid == jobs[i]){
	// resume jobs[i] in fg
      }
      else {
	cout << "job JID: " << jid << " could not be found in foreground" << endl;
      }
    }
  }
  else if (process.compare("export") == 0){
    // export
  }
  else if (process.compare("kill") == 0){
    int pidK;
    string ssignal = argv[1];
    if (ssignal.compare("-s") == 0){
      int signal = atoi(argv[2]);
      pidK = atoi(argv[3]);
      kill(pidK, signal);
    }
    else {
      //string signal = "SIGTERM";
      pidK = atoi(argv[1]);
      kill(pidK, SIGTERM);
    }
  }
  else if (process.compare("exit") == 0){
    if(argv[1] == NULL){
      exit(0); // change to status of last job
    }
    else{
      int status = stoi(argv[1]);
      if ((status >= 0)&&(status <= 255)){
	exit(status);
      }
    }
    return true;
  }
  else if (process.compare("help") == 0){
    cout << "- bg JID - Resume the stopped job JID in the background, as if it had been started with &." << endl; 
    cout << "- cd [PATH] - Change the current directory to PATH. The environmental variable HOME is the default PATH." << endl;
    cout << "- exit [N] - Cause the shell to exit with a status of N. If N is omitted, the exit status is that of the last job executed." << endl;
    cout << "- export NAME[=WORD] - NAME is automatically included in the environment of subsequently executed jobs." << endl;
    cout << "- fg JID - Resume job JID in the foreground, and make it the current job." << endl;
    cout << "- help - Display helpful information about builtin commands." << endl;
    cout << "- jobs - List current jobs. Here is an example of the desired output:" << endl;
    cout << "   JID STATUS COMMAND\n   1137 Stopped less &\n   2245 Running cat /dev/urandom | less &\n   2343 Running ./jobcontrol &" << endl;
    cout << "- kill [-s SIGNAL] PID - The kill utility sends the specied signal to the specied process or process group PID (see kill(2)). If no signal is specied, the SIGTERM signal is sent. The SIGTERM signal will kill processes which do not catch this signal. For other processes, it may be necessary to use the SIGKILL signal, since this signal cannot be caught. If PID is positive, then the signal is sent to the process with the ID specied by PID. If PID equals 0, then the signal is sent to every process in the current process group. If PID equals -1, then the signal is sent to every process for which the utility has permission to send signals to, except for process 1 (init). If PID is less than -1, then the signal is sent to every process in the process group whose ID is -PID. When the -s SIGNAL option is used, instead of sending SIGTERM, the specied signal is sent instead. SIGNAL can be provided as a signal number or a constant (e.g., SIGTERM)." << endl;
    return true;
  }
  return false;
} // isBuiltin
/**
 * Closes file descriptors
 */
void closepipe(int pipefd[2]){
  if (close(pipefd[0]) < 0){
    perror("close 0");
  } //else
  if (close(pipefd[1]) < 0){
    perror("close 1");
  } //else
} // closepipe


/**
 * Prints out prompt based on the current working directory
 */
string prompt(){
  string prompt;
  //check if pwd is part of the home directory
  if (pwd.compare(0, strlen(home), home) == 0){
    pwd = "~" + pwd.substr(strlen(home), pwd.length());
  }
  prompt = "1730sh:" + pwd + "$ ";
  return prompt;
} //getprompt
