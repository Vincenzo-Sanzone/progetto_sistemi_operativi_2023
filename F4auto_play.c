#include <sys/sem.h>
#include <sys/shm.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <errno.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

typedef struct lavoramem{
	char simbolo_giocatore, simbolo_nullo;
	int riga, colonna;
	pid_t pid;
}lavoramem;

struct messaggio{
	long priorita;
	/*0 indica la parità, 1 indica il giocatore 1, 2 il giocatore 2. 
	In caso sia il client ad inviare il messaggio indica quale giocatore ha abbandonato*/
	unsigned short vincitore; 
	bool abbandono; //True il giocatore ha abbandonato, false se non ha abbandonato
	bool tempo_scaduto; //True se il giocatore non ha fatto la sua mossa in tempo 
};

int id_file = -1;
int id_coda = -1;
int id_sem = -1; //Contine l'id del semaforo
int righe;
char **mem_tab = NULL;
lavoramem *mem_dati = NULL;

//Funzione per scollegare le risorse condivise
void rimuovi_tutto();

void errExit(char messaggio[]);

void errExit_from_remove(char messaggio[]);

int leggi();

void sigHandler(int sig);

void mysemop(int semid, int num, int op, int flg);

int main(int argc, char *argv[]){
	sigset_t block;
	if(sigfillset(&block) == -1)
		errExit("sigfill");
	if(sigdelset(&block, SIGTERM) == -1)
		errExit("sidelset");
	if(sigdelset(&block, SIGUSR1) == -1)
		errExit("sigdelset");
	if(sigprocmask(SIG_SETMASK, &block, NULL) == -1)
		errExit("sigprocmask");
	if(signal(SIGTERM, sigHandler) == SIG_ERR)
		errExit("signal");
	if(signal(SIGUSR1, sigHandler) == SIG_ERR)
		errExit("signal");

	righe = atoi(argv[1]); int colonne = atoi(argv[2]);
	char simbolo_nullo = *argv[3]; char giocatore_due = *argv[4];

	id_file = open("key.txt", O_RDONLY);
	if(id_file == -1)
		errExit("open");

	id_sem = leggi();

	//Mi collego alla memoria condivisa
	int id_mem_dati = leggi(false);
	mem_dati = (lavoramem *)shmat(id_mem_dati, NULL, 0);
	if(mem_dati == (void *) -1)
		errExit("shmat");

	//Mi collego al tabellone
	mem_tab = (char **) malloc(sizeof(char*) * righe); //Alloco nuovo spazio, per poter salvare i miei puntatori
	for(int i = 0; i < righe; i++){
		int tmp = leggi();
		mem_tab[i] =(char *) shmat(tmp, NULL, 0);
		if(mem_tab[i] == (void *)-1)
			errExit("shmat");
	}

	id_coda = leggi();
	srand(time(NULL));
	while(1){
		//Aspetto per poter effettuare la mossa
		mysemop(id_sem, 2, -1, 0);
		//Cerco una mossa valida
		bool finito = false;
		do{
			int pos_col = rand() % colonne;
			for(int i = righe - 1; i >= 0; i--){
				if(mem_tab[i][pos_col] == simbolo_nullo){
					mem_tab[i][pos_col] = giocatore_due;
					lavoramem aggiorna_dati = {.colonna = pos_col, .riga = i, .simbolo_giocatore = giocatore_due};
					*mem_dati = aggiorna_dati;
					finito = true;
					break;
				}
			}
		}while(!finito);
		//Dico che ho finito
		mysemop(id_sem, 0, 1, 0);
	}
}

void rimuovi_tutto(){
	//Mi scollego dalla memoria
	for(int i = 0; i < righe; i++){
		if(mem_tab[i] == (void *)-1)
			break;
		if(shmdt(mem_tab[i]) == -1)
			errExit_from_remove("shmdt");
	}

	//Rimuovo le memorie allocate con malloc
	if(mem_tab != NULL)
		free(mem_tab);

	//Scollego la memoria dati
	if(mem_dati != NULL && shmdt(mem_dati) == -1)
		errExit_from_remove("shmdt");

	if(id_file != -1 && close(id_file) == -1)
		errExit("close");
}

void errExit_from_remove(char messaggio[]){
	perror(messaggio);
	exit(-1);
}

void errExit(char messaggio[]){
	rimuovi_tutto();
	perror(messaggio);
	exit(-1);
}

int leggi(){
	static int offset = 0; //Mi dice dove devo andare a leggere

	//Mi assicuro che il file esista
	if(id_file == -1)
		return -1;

	//Mi sposto di offset caratteri dall'inizio
	lseek(id_file, offset, SEEK_SET);

	char buff[10]; //MAX_INT 10 cifre possibili

	//Leggo i primi 10 caratteri
	if(read(id_file, buff, 10) == -1)
		errExit("read");

	//Se il primo carattere è quello di terminazione allora non ho interi da leggere
	if(buff[0] == '\0')
		return -1;

	//Conto i numeri di caratteri letti fino a '\n' compreso
	bool flag = true;
	int count = 0;
	for( ;count < 10 && flag; count++)
		if(buff[count] == 10)
			flag = false;

	int res;
	if(!flag){ //Se ho letto '\n'
		offset += count; //L'offset aumenta del numero di caratteri letti
		char converti[count-1];
		for(int i = 0; i < count; i++){
			converti[i] = buff[i];
		}
		res = atoi(converti);
	}
	else{
		offset += count + 1; //Caso in cui abbiamo letto 10 cifre, e dobbiamo anche togliere l'a capo
		res = atoi(buff);
	}
	return res;
}

void sigHandler(int sig){
	if(sig == SIGUSR1){
		struct messaggio coda;
		if(msgrcv(id_coda, &coda, sizeof(struct messaggio) - sizeof(long), 0, 0) == -1)
			errExit("msgrcv");
		if(coda.abbandono){
			mysemop(id_sem, 0, 2, 0); //Dico al server che ho letto il messaggio
		}
		else{
			mysemop(id_sem, 0, 1, 0);	
		}
		
	}
	rimuovi_tutto();
	exit(0);
}

void mysemop(int semid, int num, int op, int flg){
	struct sembuf operazione= {.sem_num = num, .sem_op = op, .sem_flg = 0};
	do{
		errno = 0;
		semop(semid, &operazione, 1);
	}while(errno == EINTR);
	if(errno != 0)
		errExit("semop");
}