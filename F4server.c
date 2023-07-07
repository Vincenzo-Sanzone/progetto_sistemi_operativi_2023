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
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/wait.h>

#define FILA 4 // Indica quanti simboli devono essere in fila (or/ver/diag) per vincere
#define MAX_INPUT_VALUE 4000
#define ASPETTA_CTRL_C 10 // entro quanti secondi deve premere 2 volte ctrl c
#define ASPETTA_MOSSA 10  // Entro quanti secondi il giocatore deve fare la sua mossa

typedef struct lavoramem
{
	char simbolo_giocatore, simbolo_nullo;
	int riga, colonna;
	pid_t pid;
} lavoramem;

union semun
{
	int val;
	struct semid_ds *buf;
	unsigned short *array;
};

struct messaggio
{
	long priorita;
	/*0 indica la parità, 1 indica il giocatore 1, 2 il giocatore 2.
	In caso sia il client ad inviare il messaggio indica quale giocatore ha abbandonato*/
	unsigned short vincitore;
	bool abbandono;		// True il giocatore ha abbandonato, false se non ha abbandonato
	bool tempo_scaduto; // True se il giocatore non ha fatto la sua mossa in tempo
};

int righe, colonne; // riga/colonna della mia matrice
lavoramem *mem_dati = NULL;
char **mem_tab = NULL;							  // Tabellone
char simbolo_nullo, giocatore_uno, giocatore_due; // Simboli usati in partita
pid_t pid_client[2] = {-1, -1};					  // Array in cui avrò i pid dei due client
int id_file = -1;								  // id del file, inizializzato a un valore errato
int inserimenti;								  // Numero di gettoni inseriti nella matrice
int id_coda = -1;								  // id della coda dei messaggi
int id_sem = -1;								  // Id del semaforo
bool auto_play = false;							  // variabile che mi dice se c'è il gioco automatico

// Funzione in caso di errore durante l'eliminazione delle risorse
void errExit_from_remove(char messaggio[]);

// Funzione che legge da un file, e restituisce l'id letto, o -1 in caso non ci sia un id da leggere
int leggi();

/*Funzione che rilascia i semafori e le zone di memorie allocate*/
void rimuovi_tutto();

// Funzione che invia il segnale sig al pid_t[processo]
void invia_segnali(int processo, int sig);

// Funzione in caso di errore del programma
void errExit(char messaggio[]);

/*Funzione per intercettare i segnali*/
void sigHandler(int sig);

// Funzione che scrive sul file
void scrivi(int id);

/*Funzione modificata di semop, continua in caso fosse stata interotta da un interrupt.*/
void mysemop(int semid, int num, int op, int flg);

// Funzione modificata di semctl
void mysemctl(unsigned short s1, unsigned short s2, unsigned short s3);

/*Funzione che restituisce quanti simboli uguali abbiamo in diagonale
Assunzione la giocata va da 0 a righe/colonne -1*/
int diagonale(char simbolo, int counter, int giocata_riga, int giocata_colonna, int verso);

/*Funzione che restituisce quanti simboli uguali abbiamo in verticale
Assunzione la giocata va da 0 a righe/colonne -1*/
int verticale(char simbolo, int counter, int giocata_riga, int giocata_colonna, bool verso);

/*Funzione che restituisce quanti simboli uguali abbiamo in orizzontale
Assunzione la giocata va da 0 a righe/colonne -1*/
int orizzontale(char simbolo, int counter, int giocata_riga, int giocata_colonna, bool verso);

/*funzione che restituisce true in caso di vittoria, false altrimenti
Assunzione la giocata va da 0 a righe/colonne-1*/
bool vittoria(char simbolo, int giocata_riga, int giocata_colonna);

int main(int argc, char *argv[])
{
	if (argc != 5)
	{ // Controllo il corretto utilizzo del programma
		printf("Errore nell'utilizzo. Uso tipo:\n");
		printf("%s <numero_righe> <numero_colonne> <simbolo_p1> <simbolo_p2>.\n", argv[0]);
		return -1;
	}
	else
	{
		double controllo_riga = atof(argv[1]);	  // Verifico che le righe non siano un float
		double controllo_colonna = atof(argv[2]); // Verifico che le colonne non siano un float
		if (controllo_riga != (double)(int)controllo_riga || controllo_colonna != (double)(int)controllo_colonna ||
			controllo_riga < 5 || controllo_colonna < 5)
		{
			printf("Errore nell'utilizzo. Le righe e colonne devono essere interi maggiori o uguali a 5\n");
			return -2;
		}
		else if (strlen(argv[3]) != 1 || strlen(argv[4]) != 1 || *argv[3] == *argv[4])
		{
			printf("Errore nell'utilizzo. I simboli devono essere diversi e devono avere lunghezza 1\n");
			return -3;
		}
		if (controllo_riga > MAX_INPUT_VALUE)
		{
			printf("Superati i limiti del programma, le righe massime consentite sono: %d\n", MAX_INPUT_VALUE);
			return -4;
		}
	}

	// Gestisco i segnali
	if (signal(SIGINT, sigHandler) == SIG_ERR)
		errExit("signal");
	if (signal(SIGHUP, sigHandler) == SIG_ERR)
		errExit("signal");
	if (signal(SIGALRM, sigHandler) == SIG_ERR)
		errExit("signal");
	if (signal(SIGUSR1, sigHandler) == SIG_ERR)
		errExit("signal");
	if (signal(SIGUSR2, sigHandler) == SIG_ERR)
		errExit("signal");

	// Variabili contenenti informazioni generali
	righe = atoi(argv[1]);
	colonne = atoi(argv[2]);
	giocatore_uno = *argv[3]; // simbolo del giocatori 1
	giocatore_due = *argv[4]; // Simbolo del giocatore 2

	// Creo un file temporaneo, eliminandolo, in caso dovesse esistere già
	id_file = open("key.txt", O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR);
	if (errno == EEXIST)
	{
		errno = 0;
		unlink("key.txt"); // provo ad eliminiare il file
		id_file = open("key.txt", O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR);
		if (errno == EEXIST)
		{
			printf("Si prega di eliminare il file \"key.txt\", per un corretto funzionamento del programma.\n");
			return -5;
		}
	}
	if (id_file == -1)
		errExit("open");

	// Creo i semafori
	id_sem = semget(IPC_PRIVATE, 3, IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
	if (id_sem == -1)
		errExit("semget");

	// Scrivo l'id del semaforo sul file
	scrivi(id_sem);

	// Setto tutti i semafori a 0
	mysemctl(0, 0, 0);

	// Creo e collego la memoria per passare le informazioni
	int id_mem_dati = shmget(IPC_PRIVATE, sizeof(lavoramem), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
	if (id_mem_dati == -1)
		errExit("shmget");
	scrivi(id_mem_dati); // Scrivo nel file l'id
	mem_dati = (lavoramem *)shmat(id_mem_dati, NULL, 0);
	if (mem_dati == (void *)-1)
		errExit("shmat");

	// Creo e collego la memoria del mem_tab
	mem_tab = (char **)malloc(sizeof(char *) * righe);
	if (mem_tab == (void *)NULL)
		errExit("malloc");
	for (int i = 0; i < righe; i++)
	{
		int tmp = shmget(IPC_PRIVATE, sizeof(char) * colonne, IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
		if (tmp == -1)
		{
			errExit("shmget");
		}
		scrivi(tmp);
		mem_tab[i] = (char *)shmat(tmp, NULL, 0);
		if (mem_tab[i] == (void *)-1)
			errExit("shmat");
	}

	// Creo una coda di messaggi
	id_coda = msgget(IPC_PRIVATE, IPC_CREAT | S_IRUSR | S_IWUSR);
	if (id_coda == -1)
		errExit("msgget");
	scrivi(id_coda);

	// Inizializzo il carattere nullo
	{
		bool zero_gioc = (giocatore_uno == '0') || (giocatore_due == '0');
		bool uno_gioc = (giocatore_uno == '1') || (giocatore_due == '0');
		if (zero_gioc && uno_gioc)
			simbolo_nullo = '2';
		else if (zero_gioc)
			simbolo_nullo = '1';
		else
			simbolo_nullo = '0';
	}

	// Inizializzo il mem_tab
	for (int i = 0; i < righe; i++)
	{
		for (int j = 0; j < colonne; j++)
		{
			mem_tab[i][j] = simbolo_nullo;
		}
	}

	lavoramem aggiorna_dati;						 // Struttura che uso per aggiornare i dati
	aggiorna_dati.simbolo_nullo = simbolo_nullo;	 // Inserisco il carattere nullo nella struct, per condivederlo con i client
	aggiorna_dati.simbolo_giocatore = giocatore_uno; // Comunico al client il proprio simbolo
	aggiorna_dati.riga = righe * 2;					 // Inserisco il numero di righe e colonne, cosi i client ne saranno a conoscenza
	aggiorna_dati.colonna = colonne;
	aggiorna_dati.pid = getpid(); // Inserisco il mio pid
	*mem_dati = aggiorna_dati;	  // Inserisco i dati in memoria

	// Aggiorno il terzo semaforo (0, 0, 1), dando accesso al primo giocatore
	mysemop(id_sem, 2, 1, 0);

	// Aspetto che il giocatore 1 abbia finito
	mysemop(id_sem, 0, -1, 0);

	pid_client[0] = mem_dati->pid; // Salvo il pid del giocatore 1

	if (!auto_play)
	{
		// Aggiorno i dati della memoria condivisa
		aggiorna_dati.simbolo_nullo = simbolo_nullo;	 // Inserisco il carattere nullo nella struct, per condivederlo con i client
		aggiorna_dati.simbolo_giocatore = giocatore_due; // Comunico al client il proprio simbolo
		aggiorna_dati.riga = righe * 2 - 1;				 // Inserisco il numero di righe e colonne, cosi i client ne saranno a conoscenza
		aggiorna_dati.colonna = colonne;
		aggiorna_dati.pid = getpid(); // Inserisco il mio pid
		*mem_dati = aggiorna_dati;

		// Do la possibilità al secondo giocatore di connettersi
		mysemop(id_sem, 2, 1, 0);

		// Aspetto che il giocatore 2 abbia finito
		mysemop(id_sem, 0, -1, 0);

		pid_client[1] = mem_dati->pid; // Salvo il pid del giocatore 2;
	}

	do
	{
		inserimenti++; // Numero di inserimenti effettuati

		alarm(ASPETTA_MOSSA);
		// Aggiorno il semaforo 3, se siamo alla mossa pari, il semaforo 2 altrimenti
		mysemop(id_sem, (inserimenti % 2 == 0) ? 2 : 1, 1, 0);

		// Aspetto che il giocatore faccia la sua mosaa
		mysemop(id_sem, 0, -1, 0);
		alarm(0);
	} while (!vittoria(mem_dati->simbolo_giocatore, mem_dati->riga, mem_dati->colonna) &&
			 inserimenti != righe * colonne);

	if (inserimenti != righe * colonne)
	{
		// Inserisco i due messaggi in coda (uno per giocatore)
		for (int i = 0; i < 2; i++)
		{
			struct messaggio coda = {.priorita = 1, .vincitore = (inserimenti % 2 == 0) ? 2 : 1, .abbandono = false, .tempo_scaduto = false};
			if (msgsnd(id_coda, &coda, sizeof(struct messaggio) - sizeof(long), 0) == -1)
				errExit("msgsnd");
			invia_segnali(pid_client[i], SIGUSR1); // Avviso il giocatore che è arrivato il messaggio
		}
		// printf("Ha vinto il giocatore: %d\n", (mem_dati->simbolo_giocatore == giocatore_uno) ? 1 : 2);
	}
	else
	{
		// Inserisco i due messaggi in coda
		for (int i = 0; i < 2; i++)
		{
			struct messaggio coda = {.priorita = 1, .vincitore = 0, .abbandono = false, .tempo_scaduto = false};
			if (msgsnd(id_coda, &coda, sizeof(struct messaggio) - sizeof(long), 0) == -1)
				errExit("msgsnd");
			invia_segnali(pid_client[i], SIGUSR1); // Avviso il giocatore che è arrivato il messaggio
		}
		// printf("Partita finita in parita.\n");
	}
	rimuovi_tutto();

	return 0;
}

void errExit_from_remove(char messaggio[])
{
	perror(messaggio);
	exit(-1);
}

void rimuovi_tutto()
{
	int id_da_eliminare = leggi();

	// Rimuovo la memoria dati
	if (mem_dati != NULL && shmdt(mem_dati) == -1)
		errExit_from_remove("shmdt");
	id_da_eliminare = leggi();
	if (id_da_eliminare != -1 && shmctl(id_da_eliminare, IPC_RMID, NULL) == -1)
		errExit_from_remove("shmctl");

	// Rimuovo la memoria colonna
	for (int i = 0; i < righe; i++)
	{
		if (mem_tab[i] == (void *)-1)
			break;
		if (shmdt(mem_tab[i]) == -1)
			errExit_from_remove("shmdt");
		id_da_eliminare = leggi();
		if (id_da_eliminare != -1 && shmctl(id_da_eliminare, IPC_RMID, NULL) == -1)
			errExit_from_remove("shmctl");
	}

	// Rimuovo la memoria righe
	if (mem_tab != NULL)
		free(mem_tab);

	if (id_sem != -1 && ((pid_client[0] != -1) || (pid_client[1] != -1)))
		mysemop(id_sem, 0, -2, 0);

	// Rimuovo la coda dei messaggi
	if (id_coda != -1 && msgctl(id_coda, IPC_RMID, NULL) == -1)
		errExit_from_remove("msgctl");

	// Rimuovo i semafori
	if (id_sem != -1 && semctl(id_sem, 0, IPC_RMID, 0) == -1)
		errExit_from_remove("semctl");

	// Aspetto eventuali figli
	int exitstatus;
	while (wait(&exitstatus) != -1);
	if (errno != ECHILD)
		errExit("wait");

	close(id_file);	   // Chiudo il file temporaneo
	unlink("key.txt"); // Elimino il file temporaneo
}

void errExit(char messaggio[])
{
	rimuovi_tutto();
	perror(messaggio);
	exit(-1);
}

void sigHandler(int sig){
	static int counter = 0; // Dichiaro una variabile statica che tiene il conto dei ctrl c ricevuti
	if (sig == SIGINT)
	{
		static time_t tempo_precedente, tempo_attuale = 0;
		tempo_attuale = time(NULL); // Salvo il tempo in cui è arrivato ctrl c
		counter++;
		if (counter == 2)
		{
			// Se ne sono arrivati 2 verifico entro quanto tempo sono arrivati
			if (tempo_attuale - tempo_precedente < ASPETTA_CTRL_C)
			{
				invia_segnali(pid_client[0], SIGTERM);
				invia_segnali(pid_client[1], SIGTERM);
				rimuovi_tutto();
				exit(0);
			}
		}
		counter = 1;
		tempo_precedente = tempo_attuale;
		printf("\nAl prossimo CTRL + C, entro %d secondi, il programma verra terminato\n", ASPETTA_CTRL_C);
	}
	else if (sig == SIGHUP)
	{
		invia_segnali(pid_client[0], SIGTERM);
		invia_segnali(pid_client[1], SIGTERM);
		rimuovi_tutto();
		exit(0);
	}
	else if (sig == SIGALRM)
	{
		counter = 0; // Se ricevo il sigalarm allora riparto da 0 con i ctrl c
		for (int i = 0; i < 2; i++)
		{
			struct messaggio coda = {.priorita = 1, .vincitore = (inserimenti % 2 == 0) ? 1 : 2, .abbandono = false, .tempo_scaduto = true};
			if (msgsnd(id_coda, &coda, sizeof(struct messaggio) - sizeof(long), 0) == -1)
				errExit("msgsnd");
		}
		invia_segnali(pid_client[0], SIGUSR1); // Invio il segnale indicando che la partita è finita
		invia_segnali(pid_client[1], SIGUSR1); // Invio il segnale indicando che la partita è finita
		rimuovi_tutto();
		exit(0);
	}
	else if (sig == SIGUSR1)
	{ // Un giocatore ha abbandonato
		counter = 0;
		struct messaggio coda;

		// Estraggo il messaggio per vedere quale giocatore ha abbandonato
		if (msgrcv(id_coda, &coda, sizeof(struct messaggio) - sizeof(long), 0, 0) == -1)
			errExit("msgrcv");

		bool presente = false;

		// Dico all'altro che ha vinto
		if (coda.vincitore == 1 && pid_client[1] != -1)
		{
			coda.vincitore = 2;
			if (msgsnd(id_coda, &coda, sizeof(struct messaggio) - sizeof(long), 0) == -1)
				errExit("msgsnd");
			presente = true;
			invia_segnali(pid_client[1], SIGUSR1);
		}
		else if (coda.vincitore == 2 && pid_client[0] != -1)
		{
			coda.vincitore = 1;
			if (msgsnd(id_coda, &coda, sizeof(struct messaggio) - sizeof(long), 0) == -1)
				errExit("msgsnd");
			presente = true;
			invia_segnali(pid_client[0], SIGUSR1);
		}
		if (presente)
		{
			// Setto tutti i semafori a 0, se erano presenti entrambi i giocatori
			mysemctl(0, 0, 0);
		}
		else
			mysemctl(2, 0, 0);
		rimuovi_tutto();
		exit(0);
	}
	else if (sig == SIGUSR2)
	{ // Uno dei client ha il gioco automatico
		auto_play = true;
		counter = 0;
		pid_t figlio = fork();
		if (figlio == -1)
			errExit("fork");
		else if (figlio == 0)
		{
			char riga_ribaltata[10];
			char colonna_ribaltata[10];
			int counter_r = 0, counter_c = 0;
			for(int tmpr = righe, tmpc = colonne; tmpr != 0 && tmpc != 0; tmpr /= 10, tmpc /= 10){
				if(tmpr != 0)
					riga_ribaltata[counter_r++] = tmpr%10 + '0';
				if(tmpc != 0)
					colonna_ribaltata[counter_c++] = tmpc%10+ '0';
			}
			char con_righe[counter_r], con_col[counter_c];
			int fine = (counter_r > counter_c) ? counter_r:counter_c;
			for(int i = 0; i < fine;i++){
				if(counter_r != 0)
					con_righe[i] = riga_ribaltata[--counter_r];
				if(counter_c != 0)
					con_col[i] = colonna_ribaltata[--counter_c];
			}
			char simbnul[1] = {simbolo_nullo};
			char g2[1] = {giocatore_due};
			char *lista[] = {"F4auto_play", con_righe, con_col, simbnul, g2,  NULL};
			execv("./F4auto_play", lista);
			struct messaggio coda = {.priorita = 1, .vincitore = 2, .abbandono = true, .tempo_scaduto = false};
			//Carico il messaggio
			if(id_coda != -1 && msgsnd(id_coda, &coda, sizeof(struct messaggio) - sizeof(long), 0) == -1)
				errExit_from_remove("msgsnd");
			invia_segnali(getppid(), SIGUSR1); //Comunico al server che ho abbandonato
			errExit_from_remove("execl");			
		}
		pid_client[1] = figlio;
	}
}

void invia_segnali(pid_t processo, int sig)
{
	if (processo != -1)
		kill(processo, sig);
}

int leggi()
{
	static int offset = 0; // Imposto l'offset per leggere correttamente il file
	char buffer[10];	   // MAX_INT 10 cifre possibili

	if (id_file == -1) // Mi assicuro che il file esista
		return -1;

	// Mi sposto di offset byte
	lseek(id_file, offset, SEEK_SET);

	// Leggo 10 caratteri
	if (read(id_file, buffer, 10) == -1)
		errExit("read");

	// Se ho letto come primo carattere il carattere di terminazione, allora non ho altri interi da leggere
	if (buffer[0] == '\0')
		return -1;

	// Controllo se tra i caratteri letti c'è un a capo
	bool flag = true;
	int count = 0; // Variabile che segna quanti caratteri sono stati letti
	for (; count < 10 && flag; count++)
		if (buffer[count] == '\n')
			flag = false; // Se ho letto l'a capo smetto di contare

	int result; // Valore che ritornerò

	if (!flag)
	{
		offset += count; // Se ho letto l'a capo allora l'offset aumenta di count
		char tmp[count];
		for (int i = 0; i < count; i++)
		{
			tmp[i] = buffer[i];
		}
		result = atoi(tmp);
	}
	else
	{
		offset += count;	   // Altrimenti aumenta di 11 (count = 10), così alla prossima lettura partirò dal carattere corretto
		result = atoi(buffer); // Converto il numero
	}

	return result;
}

void scrivi(int id)
{
	static int offset = 0; // Indica a che posizione dobbiamo scrivere sul file
	int count = 0;
	int tmp;
	for (tmp = 0; id != 0; count++)
	{
		tmp *= 10;
		tmp += id % 10;
		id /= 10;
	}
	char buffer[count + 2]; // INT_MAX ha lunghezza 10 + 1 per '\n' + 1 per '\0'
	for (int i = 0; i < count; i++)
	{
		buffer[i] = (tmp % 10) + '0';
		tmp /= 10;
	}

	// Aggiungo a capo e il carattere di terminazione
	buffer[count] = '\n', buffer[count + 1] = '\0';

	// Mi sposto all'iesimo byte per preparare la scrittura
	if (lseek(id_file, offset, SEEK_SET) == -1)
		errExit("lseek");

	// Scrivo
	if (write(id_file, buffer, sizeof(buffer)) == -1)
		errExit("write");

	// Aggiorno il counter
	offset += count + 1; // Alla prossima scrittua vorrò sovrascrive il carattere di terminazione
}

void mysemop(int semid, int num, int op, int flg)
{
	struct sembuf operazione = {.sem_num = num, .sem_op = op, .sem_flg = 0};
	do
	{
		errno = 0;
		semop(semid, &operazione, 1);
	} while (errno == EINTR);
	if (errno != 0)
		errExit("semop");
}

void mysemctl(unsigned short s1, unsigned short s2, unsigned short s3)
{
	union semun lavorasem;
	unsigned short modifica[5] = {s1, s2, s3};
	lavorasem.array = modifica;
	if (semctl(id_sem, 0, SETALL, lavorasem) == -1)
		errExit("semctl");
}

int diagonale(char simbolo, int counter, int giocata_riga, int giocata_colonna, int verso)
{
	if (giocata_riga == -1 || giocata_riga == righe || giocata_colonna == -1 || giocata_colonna == colonne || counter >= FILA)
	{
		return counter;
	}
	else if (counter == 0)
	{
		counter++;																		// Il primo simbolo è sempre corrispondente
		int ss = diagonale(simbolo, counter, giocata_riga - 1, giocata_colonna - 1, 0); // Mi sposto su e sx
		int gd = diagonale(simbolo, counter, giocata_riga + 1, giocata_colonna + 1, 1); // Mi sposto giù e dx
		if (ss + gd - 1 >= FILA)
			return ss + gd - 1;															// Se ho già superato il punteggio, allora ho vinto
		int sd = diagonale(simbolo, counter, giocata_riga - 1, giocata_colonna + 1, 2); // Mi sposto su e dx
		int gs = diagonale(simbolo, counter, giocata_riga + 1, giocata_colonna - 1, 3); // Mi sposto giù e sx
		return sd + gs - 1;
	}
	else if (verso == 0)
	{ // Caso su e sx
		if (simbolo == mem_tab[giocata_riga][giocata_colonna])
		{
			counter++;
			return diagonale(simbolo, counter, giocata_riga - 1, giocata_colonna - 1, verso);
		}
		else
			return counter;
	}
	else if (verso == 1)
	{ // Caso giù e dx
		if (simbolo == mem_tab[giocata_riga][giocata_colonna])
		{
			counter++;
			return diagonale(simbolo, counter, giocata_riga + 1, giocata_colonna + 1, verso);
		}
		else
			return counter;
	}
	else if (verso == 2)
	{ // Caso su e dx
		if (simbolo == mem_tab[giocata_riga][giocata_colonna])
		{
			counter++;
			return diagonale(simbolo, counter, giocata_riga - 1, giocata_colonna + 1, verso);
		}
		else
			return counter;
	}
	else
	{ // Caso giù e sx
		if (simbolo == mem_tab[giocata_riga][giocata_colonna])
		{
			counter++;
			return diagonale(simbolo, counter, giocata_riga + 1, giocata_colonna - 1, verso);
		}
		else
			return counter;
	}
}

int verticale(char simbolo, int counter, int giocata_riga, int giocata_colonna, bool verso)
{
	if (giocata_riga == -1 || giocata_riga == righe || counter >= FILA)
	{
		return counter; // Passo base
	}
	else if (counter == 0)
	{																					// Sono nel simbolo che ho inserito io
		counter++;																		// Sicuramente il primo simbolo è corretto
		int rp = verticale(simbolo, counter, giocata_riga - 1, giocata_colonna, true);	// Mi sposto in alto
		int rm = verticale(simbolo, counter, giocata_riga + 1, giocata_colonna, false); // Mi sposto in basso
		return rp + rm - 1;																// Il risultato sarà gli uguali di su + gli uguali di giù -1 (il simbolo centrale è ripetuto)
	}
	else if (verso)
	{ // Mi sono spostato su
		if (simbolo == mem_tab[giocata_riga][giocata_colonna])
		{
			// Se combaciano, allora il risultato sarà 1 + i combacianti di su
			counter++;
			return verticale(simbolo, counter, giocata_riga - 1, giocata_colonna, verso);
		}
		else
			return counter; // Se non combaciano ritorno i combacianti precedenti
	}
	else
	{ // Mi sto spostando in giù
		if (simbolo == mem_tab[giocata_riga][giocata_colonna])
		{
			// Se combaciano, allora il risultato sarà 1 + i combacianti di giù
			counter++;
			return verticale(simbolo, counter, giocata_riga + 1, giocata_colonna, verso);
		}
		else
			return counter; // Se non combaciano ritorno i combacianti precedenti
	}
}

int orizzontale(char simbolo, int counter, int giocata_riga, int giocata_colonna, bool verso)
{
	if (giocata_colonna == -1 || giocata_colonna == colonne || counter >= FILA)
	{
		return counter; // Passo base
	}
	else if (counter == 0)
	{																					  // Sono nel simbolo che ho inserito io
		counter++;																		  // Sicuramente il primo simbolo è corretto
		int cp = orizzontale(simbolo, counter, giocata_riga, giocata_colonna + 1, true);  // Mi sposto a destra
		int cm = orizzontale(simbolo, counter, giocata_riga, giocata_colonna - 1, false); // Mi sposto a sinistra
		return cp + cm - 1;																  // Il risultato sarà gli uguali a sx + gli uguali a dx -1 (il simbolo centrale è ripetuto)
	}
	else if (verso)
	{ // Mi sono spostato a dx
		if (simbolo == mem_tab[giocata_riga][giocata_colonna])
		{
			// Se combaciano, allora il risultato sarà 1 + i combacianti di dx
			counter++;
			return orizzontale(simbolo, counter, giocata_riga, giocata_colonna + 1, verso);
		}
		else
			return counter; // Se non combaciano ritorno i combacianti precedenti
	}
	else
	{ // Mi sto spostando a sx
		if (simbolo == mem_tab[giocata_riga][giocata_colonna])
		{
			// Se combaciano, allora il risultato sarà 1 + i combacianti di sx
			counter++;
			return orizzontale(simbolo, counter, giocata_riga, giocata_colonna - 1, verso);
		}
		else
			return counter; // Se non combaciano ritorno i combacianti precedenti
	}
}

bool vittoria(char simbolo, int giocata_riga, int giocata_colonna)
{
	return (orizzontale(simbolo, 0, giocata_riga, giocata_colonna, false) >= FILA) ||
		   (verticale(simbolo, 0, giocata_riga, giocata_colonna, false) >= FILA) ||
		   (diagonale(simbolo, 0, giocata_riga, giocata_colonna, 0) >= FILA);
}
