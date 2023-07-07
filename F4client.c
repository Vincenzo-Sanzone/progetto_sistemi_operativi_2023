/*
VR471513
Vincenzo Sanzone
13/06/2023
*/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>

#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <linux/limits.h> //Libreria per includere PATH_MAX
#include <sys/stat.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/msg.h>
#include <sys/ipc.h>
#include <dirent.h>
#include <sys/types.h>

#define MAX_INPUT_VALUE 4000
#define ASPETTA_SERVER 10

typedef struct lavoramem{
	char simbolo_giocatore, simbolo_nullo;
	int riga, colonna;
	pid_t pid;
}lavoramem;

union semun {
	int val;
	struct semid_ds * buf;
	unsigned short * array;
};

struct messaggio{
	long priorita;
	unsigned short vincitore; //0 indica la parità, 1 indica il giocatore 1, 2 il giocatore 2
	bool abbandono; //True il giocatore ha abbandonato, false se non ha abbandonato
	bool tempo_scaduto; //True se il giocatore non ha fatto la sua mossa in tempo 
};

int righe, colonne;
int id_file = -1;
int id_coda = -1; //Contiene l'id della coda dei messaggi
int id_sem = -1; //Contine l'id del semaforo
char **mem_tab = NULL;
int *indice_giocata_riga = NULL;
lavoramem *mem_dati = NULL;
pid_t pid_server = -1;
bool giocatore_uno = false;
DIR *directory = NULL; 

//Funzione in caso di errore durante l'eliminazione delle risorse 
void errExit_from_remove(char messaggio[]);

//Funzione che rilascia le memorie allocate
void rimuovi_tutto();

//Funzione in caso di un errore
void errExit(char messaggio[]);

void sigHandler(int sig);

void mysemop(int semid, int num, int op, int flg);

//Funzione che invia un segnale al server
void invia_segnale(int sig);

//Funzione che stampa il risultato della partita
void stampa_esito();

/*Funzione che legge da file e restituisce l'intero scritto, o -1 in caso non esista un intero.
Reset indica se vogliamo leggere dall'inizio oppure da dove eravamo rimasti.*/
int leggi(bool reset);

//Funzione che stampa il tabellone
void stampa_tabellone(char simbolo_nullo, int *indice_giocata_riga);

//Funzione che valuta ed eventualmente inserisce, la validità di una giocata. True: andata a buon fine, false altrimenti
bool gioca(int giocata_colonna, int * indice_giocata_riga);

int main(int argc, char *argv[]){
	if(argc < 2){
		printf("Errore nell'uso del programma.\n");
		printf("Esempio d'uso: %s <nome_giocatore> * (aggiungere per la modalita gioco automatica)\n", argv[0]);
		return -1;
	}
	else if(argc >= 3){
		char path_to_dir[PATH_MAX];
		if(getcwd(path_to_dir, PATH_MAX) == NULL)
			errExit("getcwd");
		directory = opendir(path_to_dir); //Apro la directory dove è stato eseguito il programma
		if(directory == NULL)
			errExit("opendir");
		struct dirent *cartella = readdir(directory) ;
		int count = 0;
		
		while(cartella != NULL){
			if(errno != 0)
				errExit("readdir");
			if(cartella->d_type == DT_REG){
				count++;
				bool flag = false;
				for(int i = 0; i < argc - 2; i++){
					if(strcmp(cartella->d_name, argv[i+2]) == 0){
						flag = true;
						break;
					}
				}
				if(!flag){
					printf("Errore nell'uso del programma.\n");
					printf("Esempio d'uso: .%s <nome_giocatore> * (aggiungere per la modalita gioco automatica)\n", argv[0]);
					rimuovi_tutto(); //Chiudo la directory
					return -2;
				}

			}
			if(strcmp(cartella->d_name, argv[1]) == 0){
				printf("Errore nell'uso del programma.\n");
				printf("Esempio d'uso: .%s <nome_giocatore> * (aggiungere per la modalita gioco automatica)\n", argv[0]);
				rimuovi_tutto(); //Chiudo la directory
				return -3;
			}
			cartella = readdir(directory);
		}
		if(closedir(directory) == -1)
			errExit("closedir");
		directory = NULL;
		if(argc - count + 1 > 3){
			printf("Errore nell'uso del programma.\n");
			printf("Esempio d'uso: %s <nome_giocatore> * (aggiungere per la modalita gioco automatica)\n", argv[0]);
			return -4;
		}
	}

	//Gestisco i segnali
	if(signal(SIGTERM, sigHandler) == SIG_ERR)
		errExit("signal");
	if(signal(SIGUSR1, sigHandler) == SIG_ERR)
		errExit("signal");
	if(signal(SIGINT, sigHandler) == SIG_ERR)
		errExit("signal");
	if(signal(SIGHUP, sigHandler) == SIG_ERR)
		errExit("signal");

	do{
		errno = 0;
		if(id_file != -1)
			close(id_file);
		id_file = open("key.txt", O_RDONLY); //Apro il file temporaneo
		
		//Se il file non esiste allora il server non si è connesso
		while(errno == ENOENT){
			printf("Il server non è pronto, riprovo tra %d secondi.\n", ASPETTA_SERVER);
			sleep(ASPETTA_SERVER);
			errno = 0;
			id_file = open("key.txt", O_RDONLY);
		}
		if(id_file == -1)
			errExit("open");
		
		id_sem = leggi(true);
		
		//Aspetto che il server mi faccia entrare
		struct sembuf lavorasem = {.sem_num = 2, .sem_op = -1, .sem_flg = 0};
		semop(id_sem, &lavorasem, 1);
		if(errno == EINVAL){
			printf("Il server non è pronto, riprovo tra %d secondi.\n", ASPETTA_SERVER);
			sleep(ASPETTA_SERVER);
		}
		else if(errno != 0)
			errExit("semop");

	}while(errno == EINVAL);

	//dichiarazione variabili generali
	bool gioco_automatico = (argc != 2) ? true:false;
	char simbolo_giocatore, simbolo_nullo;
	
	//Mi collego alla memoria condivisa
	int id_mem_dati = leggi(false);
	mem_dati = (lavoramem *)shmat(id_mem_dati, NULL, 0);
	if(mem_dati == (void *) -1)
		errExit("shmat");
	
	//Aggiorno le informazioni 
	simbolo_giocatore = mem_dati->simbolo_giocatore;
	simbolo_nullo = mem_dati->simbolo_nullo;
	pid_server = mem_dati->pid;
	colonne = mem_dati->colonna;
	mem_dati->pid = getpid();

	//Capisco se sono il giocatore 1 o il giocatore 2 e salvo il nr di righe
	if((mem_dati->riga) % 2 == 0){
		righe = (mem_dati->riga) / 2; 
		giocatore_uno = true;
	}
	else{
		righe = ((mem_dati->riga) + 1) / 2;
		giocatore_uno = false;
	}

	//Mi collego al tabellone
	mem_tab = (char **) malloc(sizeof(char*) * righe); //Alloco nuovo spazio, per poter salvare i miei puntatori
	for(int i = 0; i < righe; i++){
		int tmp = leggi(false);
		mem_tab[i] =(char *) shmat(tmp, NULL, 0);
		if(mem_tab[i] == (void *)-1)
			errExit("shmat");
	}

	//Leggo l'id della coda dei messaggi
	id_coda = leggi(false);

	indice_giocata_riga = (int *) malloc(sizeof(int) * colonne);
	
	//Gestisco il gioco automatico
	if(gioco_automatico){
		//Comunico che è attivo il gioco automatico
		invia_segnale(SIGUSR2);
	}

	//Dico al server che ho finito
	mysemop(id_sem, 0, 1, 0);
	
	if(giocatore_uno && !gioco_automatico)
		printf("Aspettando il secondo giocatore.\n");

	do{
		//Se sono il primo giocatore aspetterò al semaforo 2, altrimenti al 3 
		mysemop(id_sem, (giocatore_uno) ? 1:2, -1, 0);

		//Stampo il tabellone
		stampa_tabellone(simbolo_nullo, indice_giocata_riga);
		int gio_col;
		do{
			printf("Inserire un numero tra 1 e %d. La colonna selezionata non deve essere piena\n", colonne);
			scanf("%d", &gio_col); //Prendo dall'utente la giocata
		}while(!gioca(gio_col, indice_giocata_riga));

		//Aggiorno il tabellone
		mem_tab[indice_giocata_riga[gio_col-1]][gio_col-1] = simbolo_giocatore;

		//Inserisco i dati aggiornati nella memoria
		mem_dati->simbolo_giocatore = simbolo_giocatore;
		mem_dati->colonna = gio_col-1;
		mem_dati->riga = indice_giocata_riga[gio_col-1];

		stampa_tabellone(simbolo_nullo, indice_giocata_riga); //Stampa la matrice di gioco dopo che la mossa è stata effettuata
		//Dico al server che ho finito
		mysemop(id_sem, 0, 1, 0);
		
	}while(1);

	return 0;
}


void errExit_from_remove(char messaggio[]){
	perror(messaggio);
	exit(-1);
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
	if(indice_giocata_riga != NULL)
		free(indice_giocata_riga);

	//Scollego la memoria dati
	if(mem_dati != NULL && shmdt(mem_dati) == -1)
		errExit_from_remove("shmdt");

	if(id_file != -1 && close(id_file) == -1)
		errExit("close");

	if(directory != NULL && closedir(directory) == -1)
		errExit("closedir");	
}

void errExit(char messaggio[]){
	rimuovi_tutto();
	perror(messaggio);
	exit(-1);
}

void sigHandler(int sig){
	if(sig == SIGTERM){
		printf("Gioco terminato dall'esterno.\n");
		if(id_sem != -1)
			mysemop(id_sem, 0, 2, 0);
		rimuovi_tutto();
		exit(0);
	}
	else if(sig == SIGUSR1){ //Segnale che indica la fine della partita
		stampa_esito();
		rimuovi_tutto();
		exit(0);
	}
	else if(sig == SIGHUP || sig == SIGINT){
		struct messaggio coda = {.priorita = 1, .vincitore = (giocatore_uno) ? 1:2, .abbandono = true, .tempo_scaduto = false};
		//Carico il messaggio
		if(id_coda != -1 && msgsnd(id_coda, &coda, sizeof(struct messaggio) - sizeof(long), 0) == -1)
			errExit("msgsnd");
		invia_segnale(SIGUSR1); //Comunico al server che ho abbandonato
		rimuovi_tutto();
		exit(0);
	}
}

void stampa_esito(){;
	struct messaggio coda;
	if(msgrcv(id_coda, &coda, sizeof(struct messaggio) - sizeof(long), 0, 0) == -1)
		errExit("msgrcv");
	if(coda.abbandono){
		printf("Hai vinto! Il tuo avversario ha abbandonato\n");
		mysemop(id_sem, 0, 2, 0); //Dico al server che ho letto il messaggio
		return;
	}
	else if(coda.tempo_scaduto){
		if((coda.vincitore == 1 && giocatore_uno) || (coda.vincitore == 2 && !giocatore_uno))
			printf("Hai vinto! Il tuo avversario non ha effettuato la mossa in tempo.\n");
		else
			printf("Hai perso! Non hai effettuato la mossa in tempo.\n");
	}
	else if(coda.vincitore){
		if((coda.vincitore == 1 && giocatore_uno) || (coda.vincitore == 2 && !giocatore_uno))
			printf("Hai vinto!\n");
		else
			printf("Hai perso!\n");
	}
	else
		printf("La partita è finita in parita.\n");
	mysemop(id_sem, 0, 1, 0);
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

void invia_segnale(int sig){
	if(pid_server != -1)
		kill(pid_server, sig);
}

int leggi(bool reset){
	static int offset = 0; //Mi dice dove devo andare a leggere

	//Mi assicuro che il file esista
	if(id_file == -1)
		return -1;
	
	if(reset)
		offset = 0;

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

void stampa_tabellone(char simbolo_nullo, int *indice_giocata_riga){
	printf("-----");
	for(int i = 1; i < colonne; i++){
		printf("----");
	}
	printf("\n");
	for(int i = 0; i < righe; i++){
		printf("|");
		for(int j = 0; j < colonne; j++){
			if(mem_tab[i][j] != simbolo_nullo){
				printf(" %c ", mem_tab[i][j]);
				if(i == 0)
					indice_giocata_riga[j] = -1; //Piena
			}
			else{
				indice_giocata_riga[j] = i;
				printf("   ");
			}
			printf("|");
		}
		printf("\n");
		printf("-----");
		for(int i = 1; i < colonne; i++){
			printf("----");
		}
		printf("\n");  
	}
}

bool gioca(int giocata_colonna, int * indice_giocata_riga){
	return giocata_colonna <= colonne && giocata_colonna > 0 && indice_giocata_riga[giocata_colonna-1] != -1;
}
