#define _POSIX_C_SOURCE 199506L
#define _XOPEN_SOURCE
#define _XOPEN_SOURCE_EXTENDED 1

/*#define DEBUG*/
#define BUFFER_SIZE 513
#define SHELL_TEXT "dsh"
#define SHELL_COLOR 32 /* zelena */
#define DEV_NULL "/dev/null"
#define NO_REDIRECTION 0
#define REDIRECT_OUTPUT 1
#define REDIRECT_INPUT 2

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

char *buffer;
volatile sig_atomic_t program_exit = 0; /* ukoncenie programu */
volatile sig_atomic_t is_bgr_proc = 0;  /* urcuje ci ide o background process */
pid_t child;                            /* proces na pozadi */
pthread_t thread_read,thread_exec;
pthread_cond_t cond;
pthread_mutex_t mutex;

typedef struct parsed_cmd_s{
    char *argv[BUFFER_SIZE];      /* naparsovane argumenty */
    int argv_length;              /* pocet argumentov */
    int background;               /* spustit na pozadi */
    int redirect;                 /* typ presmerovania */
    int redirect_pos;             /* pozicia znaku presmerovania */
    int amp_pos;                  /* pozicia ampers. presmerovania & inak -1 */
    int stdout;
} parsed_cmd_t;


void sig_handler(int sig);
void *read_input(void *p);
void *exec_cmd(void *p);
void call_cmd(void);
int parse_buffer(char *buffer, parsed_cmd_t *flags);
void call_execvp(parsed_cmd_t flags);
void redirect_input(parsed_cmd_t flags);
void redirect_output(parsed_cmd_t flags);
void run_background(parsed_cmd_t flags);
int get_char_position(char *text, char search_char);
void debug_parsed_cmd(parsed_cmd_t flags);
void my_exect(parsed_cmd_t flags);

void sig_handler(int sig){

    /* ukonci beziaci proces na popradi */
    if(sig == SIGCHLD){
        if(is_bgr_proc){
            #ifdef DEBUG
                printf("\n\nbackground process \n\n");
            #endif
            is_bgr_proc = 0;
            fflush(stdout);
        }else{
             #ifdef DEBUG
                printf("\n\nforground process \n\n");
            #endif
            /* ak nie je background process, tak pockaj */
            wait(NULL);
        }
    }
}

int main(int argc, char* argv[], char **envp){

    struct sigaction sigact;
    pthread_attr_t attr;
    int res;

    if((buffer = (char *)calloc(BUFFER_SIZE,sizeof(char))) == NULL){
        printf("buffer mallock error\n");
        return 1;
    }

    sigact.sa_flags = 0;
    sigact.sa_handler = sig_handler;
    sigemptyset(&sigact.sa_mask);

    if(sigaction(SIGINT,&sigact,NULL)){
		printf("sigaction()");
		return 1;
	}


    /* vycisti obrazovku a zobrazi shell */
    if(fork() == 0) {
		execvp("clear", argv);
		exit(1);
	} else {
		wait(NULL);
	}

	if((res = pthread_mutex_init(&mutex,NULL)) != 0){
        printf("pthread_mutex_init() error %d\n",res);
        return 1;
	}

    if((res = pthread_attr_init(&attr)) != 0){
        printf("pthread_attr_init() error %d\n",res);
        return 1;
    }

    if((res = pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_JOINABLE)) != 0){
        printf("pthread_attr_setdetachstate() error %d\n",res);
        return 1;
    }

    if((res = pthread_cond_init(&cond,NULL)) != 0){
        printf("pthread_cond_init() error %d\n",res);
        return 1;
    }

    /* vlakno cita vstup od uzivatela */
    if((res = pthread_create(&thread_read,&attr,read_input,NULL)) != 0){
        printf("pthread_create() error %d\n",res);
        return 1;
    }

    /* vlakno spracovava jednotlive prikazy */
    if((res = pthread_create(&thread_exec,&attr,exec_cmd,NULL)) != 0){
        printf("pthread_create() error %d\n",res);
        return 1;
    }

    /* atributy uz netreba */
    if((res = pthread_attr_destroy(&attr)) != 0){
        printf("pthread_attr_destroy() error %d\n",res);
        return 1;
    }

    void *retval_read;

    if ((res = pthread_join(thread_read,&retval_read)) != 0){
        printf("pthread_join() error %d\n",res);
        return 1;
    }

    void *retval_exec;

    if ((res = pthread_join(thread_exec,&retval_exec)) != 0){
        printf("pthread_join() error %d\n",res);
        return 1;
    }

	return 0;
}

void *read_input(void *p){

    int rlen;

    while(1){

        pthread_mutex_lock(&mutex);

        /* ak nie je buffer prazdny, tak cakaj */
        while(strlen(buffer) > 0)
                pthread_cond_wait(&cond,&mutex);


        /* zobrazi shell text */
        printf("\033[%dm\033[1m%s>\033[m\033[m ",SHELL_COLOR,SHELL_TEXT);
        fflush(stdout);

        /* nacitaj vstup a ak je prilis dlhy tak chyba */
        if((rlen = read(STDIN_FILENO,buffer,BUFFER_SIZE)) == -1){
            exit(EXIT_FAILURE);
        }

        /* ukoncenie ctrl+d */
        if(buffer[0] == 0){
            exit(EXIT_SUCCESS);
        }

        buffer[rlen] = 0;

        /* ak je spravne velkost vstupu moze sa spracovat */
        if(rlen < BUFFER_SIZE){
            pthread_cond_signal(&cond);
        }else{
            printf("Error: Input is too long %d \n",rlen);

            /* vyprazdni stdin a buffer */
            char a;
            while(read(STDIN_FILENO,&a,1) != -1){
                if(a == '\n') break;
            }


            buffer = (char *)malloc(sizeof(char)*BUFFER_SIZE);
            /*memset(buffer,0,BUFFER_SIZE);*/
        }

        pthread_mutex_unlock(&mutex);
    }

    return (void *) 0;
}

void *exec_cmd(void *p){

    while(1){

        /* treba pockat na signal od input vlakna */
        pthread_mutex_lock(&mutex);


        /* ak je prazdny buffer tak cakaj */
        while(strlen(buffer) == 0)
            pthread_cond_wait(&cond,&mutex);


        call_cmd();

        /* vyprazdni buffer
         * buffer[0] = 0; - vyhadzuje segfault
         * TODO: treba vymysliet nieco rozumensie
         */

        buffer = (char *)malloc(sizeof(char)*BUFFER_SIZE);



        /* posli signal vlaknu ze moze pokracovat v nacitani */
        pthread_cond_signal(&cond);

        pthread_mutex_unlock(&mutex);
    }

    return (void *) 0;
}


void call_cmd(void){


    /* parsovanie prikazu */
     parsed_cmd_t flags;

    /* prazdne prikazy nespracovavaj */
    /*if(strlen(buffer) < 2) return;*/
    if(buffer[0] == '\n') {
        buffer = (char *)calloc(BUFFER_SIZE,sizeof(char));
        return;
    }


    #ifdef DEBUG
        printf("buffer:%s",buffer);
    #endif


    /* naparsuje buffer */
    parse_buffer(buffer,&flags);


    #ifdef DEBUG
        debug_parsed_cmd(flags);
    #endif

    /* Interne prikazy */

    /* ak je prikaz cd */
    if(strcmp(flags.argv[0],"cd") == 0){
        if(chdir(flags.argv[1]) != 0){
            perror("dsh: cd");
        }
        return;
    }


    if(strcmp(flags.argv[0],"exit") == 0){
        exit(EXIT_SUCCESS);
    }


    /* ostatne prikazy */
    call_execvp(flags);
}


/** Naparsuje buffer a hodnoty zapise do struktury */
int parse_buffer(char *buffer, parsed_cmd_t *flags){

    /* kontorla inicializacie */
    if(flags == NULL){
        printf("uninicialized struct\n");
        return -1;
    }

    int j,i = 0;
    char *ret_token;        /* naparsovany parameter */
    char *rest = (char *)malloc(sizeof(char)*BUFFER_SIZE);        /* treba inicializovat inak warning */

    flags->background = 0; /* default hodnota - spusti v popredi */
    flags->amp_pos = get_char_position(buffer,'&');/* zisti poziciu ampers. */
    if(flags->amp_pos != -1) {
        buffer[flags->amp_pos] = 0; /* odstran znak & */
        flags->background = 1;      /* nastav priznak spustenia v pozadi */
    }


    /* prechadza cely retazec a vytvara pole prikazu a jeho parametrov */
    while((ret_token = strtok_r(buffer, " ", &rest)) != NULL){
        /* odstrani znak noveho riadku */
        if(ret_token[strlen(ret_token) - 1] == '\n'){
            ret_token[strlen(ret_token) - 1] = '\0';
        }

        flags->argv[i++] = ret_token;
        buffer = rest;
    }

    flags->redirect_pos = -1;
    flags->redirect = 0;    /* default hodnota */
    flags->argv[i] = NULL;  /* na poslednu poziciu NULL */
    flags->argv_length = i; /* nastav pocet argumentov */
    flags->stdout = dup(STDOUT_FILENO);

    /* kontrola ci parameter neobsahuje presmerovanie vystupu/vstupu */
    for(j = 0; j < flags->argv_length; j++){

        /* presmerovanie vystupu */
        if(strcmp(flags->argv[j],">") == 0){
            flags->redirect = REDIRECT_OUTPUT;
            flags->redirect_pos = j;
            return 1;
        }

        /* presmerovanie vystupu */
        if(strcmp(flags->argv[j],"<") == 0){
            flags->redirect = REDIRECT_INPUT;
            flags->redirect_pos = j;
            return 1;
        }
    }

    return 1;
}

void debug_parsed_cmd(parsed_cmd_t flags){

    int j;

    printf("--------------DEBUG-------------\n");


    for(j = 0; j < flags.argv_length; j++){
        printf("argv %d: %s\n",j,flags.argv[j]);
    }
    printf("\n");
    printf("amp_pos:%d\n",flags.amp_pos);
    printf("argv_length:%d\n",flags.argv_length);
    printf("background:%d\n",flags.background);
    printf("redirect:%d\n",flags.redirect);
    printf("redirect pos: %d\n",flags.redirect_pos);

    printf("--------------------------------\n");


}

void my_exect(parsed_cmd_t flags){

}

void call_execvp(parsed_cmd_t flags){

    pid_t id;
    struct sigaction sigchild;
    is_bgr_proc = 0;    /* default sa spusta vzdy na popredi */


    /* handler na osetrenie ukoncenia procesu */
    sigchild.sa_flags = 0;
    sigchild.sa_handler = sig_handler;
    sigemptyset(&sigchild.sa_mask);

    if(sigaction(SIGCHLD,&sigchild,NULL)){
        printf("sigaction()");
        exit(EXIT_FAILURE);
    }

    if(flags.background){
        is_bgr_proc = 1;
    }

    /* vykonanie prikazov */
    if((id = fork()) == -1){
        printf("fork error\n");
        exit(EXIT_FAILURE);
    }

    /* child vykona prikaz a rodic pocka na jeho dokoncenie */
    if(id == 0){
        if(flags.redirect == REDIRECT_INPUT){
            redirect_input(flags);
        }else if(flags.redirect == REDIRECT_OUTPUT){
            redirect_output(flags);
        }
        else {/*vsetky ostatne prikazy */

                /* ak sa spusta na pozadi vytvor novy proces */
                if(flags.background){
                    run_background(flags);
                }else{/* inak normalne spusti na popredi */
                    if(execvp(flags.argv[0],flags.argv) == -1){
                        printf("%s: %s: command not found\n",SHELL_TEXT,flags.argv[0]);
                        exit(EXIT_FAILURE);
                    }
                }

        }
    }else{/* parent */
        /* cakaj len ak je proces v popredi */
        if(!flags.background){
            pause();
        }else{

        }
    }

    return;
}

void redirect_output(parsed_cmd_t flags){

    /* treba vytvorit novy subor a zapisat vystup prikazu do suboru */
    int desc = open(flags.argv[flags.redirect_pos+1],O_CREAT|O_WRONLY,S_IWUSR|S_IRUSR|S_IRGRP|S_IROTH);

    if(desc == -1){
        perror("open file");
        return;
    }

    /* presmerovanie stdout do suboru */
    dup2(desc,STDOUT_FILENO);

    /* potrebujeme len prikaz pred > */
    flags.argv[flags.redirect_pos] = NULL;

    if(flags.background){
        run_background(flags);
    }else{
        /* zavola prikaz */
        if(execvp(flags.argv[0],flags.argv) == -1){
            /* chybu treba zobrazit */
            dup2(STDERR_FILENO,STDOUT_FILENO);
            printf("%s: %s: command not found\n",SHELL_TEXT,flags.argv[0]);
            exit(EXIT_FAILURE);
        }
    }

    /* zatvor subor */
    close(desc);


    return;

}

void redirect_input(parsed_cmd_t flags){

    /* treba otvorit subor a premesmerovovat ho na stdin */
    int desc = open(flags.argv[flags.redirect_pos+1],O_RDONLY);

    if(desc == -1){
        perror(SHELL_TEXT);
        exit(EXIT_FAILURE);
    }

    /* presmerovanie na vstup */
    dup2(desc,STDIN_FILENO);

    /* potrebujeme len prikaz pred > */
    flags.argv[flags.redirect_pos] = NULL;

    if(flags.background){
        run_background(flags);
    }else{
        /* zavola prikaz */
        if(execvp(flags.argv[0],flags.argv) == -1){
            printf("%s: %s: command not found\n",SHELL_TEXT,flags.argv[0]);
            exit(EXIT_FAILURE);
        }
    }
    /* zatvor subor */
    close(desc);


    return;
}

void run_background(parsed_cmd_t flags){


    int pipefd[2];

    if(pipe(pipefd) == -1) {
        perror("pipe");
        exit(EXIT_FAILURE);
    }

    pid_t grand_child = fork();

    if(grand_child == -1){
        perror("fork");
        exit(EXIT_FAILURE);
    }

    if(grand_child == 0){ /*child*/
        close(pipefd[0]);

        /* ak sa nepresmerovanva do suboru, tak nevypisuj*/
        if(flags.redirect == NO_REDIRECTION){
            /* ak je bacground tak nevypisuj */
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
        }

        /* nech nepyta vstup */
        int null = open(DEV_NULL,O_RDWR);
        dup2(null,STDIN_FILENO);

        if(execvp(flags.argv[0],flags.argv) == -1){
            printf("%s: %s: command not found\n",SHELL_TEXT,flags.argv[0]);
            exit(EXIT_FAILURE);
        }


    }else { /* parent */

        pause();
        close(pipefd[1]);

        /* ak sa premerovava tak treba vypisat na stdout nie do suboru */
        if(flags.redirect > NO_REDIRECTION){
            dup2(flags.stdout,STDOUT_FILENO);
        }


         /* zobrazi hlasku o ukonceni */
         printf("\nDone: pid=[%d]\n",grand_child);

        /* vypis zachyteny stdout */
        char buf;
        while(read(pipefd[0], &buf, 1) > 0){
            if(write(STDOUT_FILENO, &buf, 1) == -1){
                perror("write");
                exit(EXIT_FAILURE);
            }
        }

        /* treba vpisat aj prompt nech to trocha vyzera */
        char *prompt = "\033[32m\033[1mdsh>\033[m\033[m ";
        if(write(STDOUT_FILENO,prompt,strlen(prompt)) == -1){
            perror("write");
            exit(EXIT_FAILURE);
        }

        close(pipefd[0]);
    }


    return;
}

int get_char_position(char *text, char search_char){

  char *pch;
  int pos;

  if((pch = strrchr(text,search_char)) == NULL){
        return -1;
  }

  pos = pch - text;

  return pos;
}
