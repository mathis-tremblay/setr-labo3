/******************************************************************************
 * Laboratoire 3
 * GIF-3004 Systèmes embarqués temps réel
 * Hiver 2026
 * Marc-André Gardner
 * 
 * Fichier implémentant les fonctions de communication inter-processus
 ******************************************************************************/

#define _GNU_SOURCE // pour pouvoir utiliser extensions GNU POSIX (comme pthread_mutexattr_setprotocol)

#include "commMemoirePartagee.h"


// TODO: implementez ici les fonctions decrites dans commMemoirePartagee.h


/* pour référence
    struct memPartage{
        int fd;                         // Descripteur de fichier retourné par shm_open
        struct memPartageHeader *header;// Pointeur vers le header dans la mémoire partagée
        size_t tailleDonnees;           // Taille de la zone de données (après le header)
        unsigned char* data;            // Pointeur vers la zone de données (après le header)
};
*/

// Appelée au début du programme pour l'initialisation de la zone mémoire (cas du lecteur).
// Reçoit un pointeur vers une structure memPartage _vide_.
// Cette fonction doit _remplir_ cette structure avec les informations nécessaires
// une fois la mémoire partagée initialisée.
int initMemoirePartageeLecteur(const char* identifiant,
                                struct memPartage *zone)
{
    // Étape 1: Tenter d'ouvrir le fichier virtuel de mémoire partagée
    // Réessayer si ENOENT (fichier inexistant)
    int fd = -1;
    while (fd == -1) {
        fd = shm_open(identifiant, O_RDONLY, 0666);
        if (fd == -1 && errno == ENOENT) {
            usleep(DELAI_INIT_READER_USEC);
        } else if (fd == -1) {
            perror("shm_open");
            return -1;
        }
    }
    
    // Étape 2: Vérifier la taille et valider qu'elle >= sizeof(struct memPartageHeader)
    struct stat sb;
    while (1) {
        if (fstat(fd, &sb) == -1) {
            perror("fstat");
            close(fd);
            return -1;
        }
        
        if (sb.st_size >= (off_t)sizeof(struct memPartageHeader)) {
            break;  // Taille valide : prochaine étape
        }
        
        usleep(DELAI_INIT_READER_USEC);  // Attendre que l'écrivain initialise
    }
    
    // Étape 3: Utiliser mmap pour accéder au fichier
    void *addr = mmap(NULL, sb.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return -1;
    }
    
    // Remplir la structure memPartage
    zone->fd = fd;
    zone->header = (struct memPartageHeader *)addr;
    zone->tailleDonnees = sb.st_size - sizeof(struct memPartageHeader);
    zone->data = (unsigned char *)addr + sizeof(struct memPartageHeader);
    
    // Étape 4: Attendre que etat soit différent de ETAT_NON_INITIALISE
    while (zone->header->etat == ETAT_NON_INITIALISE) {
        usleep(DELAI_INIT_READER_USEC);
    }
    
    return 0;
}

// Appelée au début du programme pour l'initialisation de la zone mémoire (cas de l'écrivain).
// Reçoit un pointeur vers une structure memPartage _vide_.
// Cette fonction doit _remplir_ cette structure avec les informations nécessaires
// une fois la mémoire partagée initialisée.
// Reçoit également un pointeur vers une struct videoInfos initialisée avec les valeurs
// correctes pour cette exécution (la bonne largeur et hauteur de l'image, etc.).
// Cette fonction doit initialiser la zone mémoire avec les valeurs correspondantes
// (par exemple, la taille des données correspond au nombre d'octets requis pour contenir
// UNE image, qui peut être déduit à partir des valeurs contenues dans la struct videoInfos).
int initMemoirePartageeEcrivain(const char* identifiant,
                                struct memPartage *zone,
                                struct videoInfos *infos)
{
    // 1. Créer et ouvrir le fichier virtuel de mémoire partagée en utilisant shm_open
    int fd = shm_open(identifiant, O_CREAT, 0666);
    if (fd == -1) {
        perror("shm_open");
        return -1;
    }

    // 2. Agrandir ce fichier à la bonne taille (la taille de l’en-tête vu plus haut + la taille d’une trame) avec ftruncate
    // Calculer la taille totale nécessaire (en-tête + données)
    size_t tailleTotale = sizeof(struct memPartageHeader) + (infos->largeur * infos->hauteur * infos->canaux);
    
    if (ftruncate(fd, tailleTotale) == -1) {
        perror("ftruncate");
        close(fd);
        return -1;
    }
    
    // 3. Utiliser mmap pour pouvoir accéder à ce fichier via un pointeur et configurer le fait qu’il s’agit d’une mémoire partagée entre deux processus
    void *addr = mmap(NULL, tailleTotale, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return -1;
    }

    zone->fd = fd;
    zone->header = (struct memPartageHeader *)addr;
    zone->tailleDonnees = tailleTotale - sizeof(struct memPartageHeader);
    zone->data = (unsigned char *)addr + sizeof(struct memPartageHeader);
    
    zone->header->infos = *infos;
    
    // 4. Initialiser le mutex et les deux conditions, en prenant soin de leur donner les attributs leur permettant d’être partagés entre processus et de gérer l’inversion de priorité
    pthread_mutexattr_t mutexattr;
    if (pthread_mutexattr_init(&mutexattr) != 0) {
        perror("pthread_mutexattr_init");
        close(fd);
        return -1;
    }
    if (pthread_mutexattr_setpshared(&mutexattr, PTHREAD_PROCESS_SHARED) != 0) {
        perror("pthread_mutexattr_setpshared");
        pthread_mutexattr_destroy(&mutexattr);
        close(fd);
        return -1;
    }
    if (pthread_mutex_init(&zone->header->mutex, &mutexattr) != 0) {
        perror("pthread_mutex_init");
        pthread_mutexattr_destroy(&mutexattr);
        close(fd);
        return -1;
    }
    pthread_mutexattr_destroy(&mutexattr);
    
    pthread_condattr_t condattr;
    if (pthread_condattr_init(&condattr) != 0) {
        perror("pthread_condattr_init");
        close(fd);
        return -1;
    }
    if (pthread_condattr_setpshared(&condattr, PTHREAD_PROCESS_SHARED) != 0) {
        perror("pthread_condattr_setpshared");
        pthread_condattr_destroy(&condattr);
        close(fd);
        return -1;
    }
    if (pthread_cond_init(&zone->header->condEcrivain, &condattr) != 0) {
        perror("pthread_cond_init (condEcrivain)");
        pthread_condattr_destroy(&condattr);
        close(fd);
        return -1;
    }
    if (pthread_cond_init(&zone->header->condLecteur, &condattr) != 0) {
        perror("pthread_cond_init (condLecteur)");
        pthread_condattr_destroy(&condattr);
        close(fd);
        return -1;
    }

    pthread_condattr_destroy(&condattr);

    // 5. Faire passer la variable etat à la valeur ETAT_PRET_SANS_DONNEES pour indiquer que l’initialisation est terminée
    zone->header->etat = ETAT_PRET_SANS_DONNEES;
    
    return 0;
}

// Appelée par le lecteur pour se mettre en attente de données sur la zone mémoire partagée
// Lorsque cette fonction retourne, le mutex devrait être verrouillé par le processus en cours!
// Est appelée par la portion lecteur d’un processus lorsqu’il attend que des 
// données soient disponibles sur la zone mémoire partagée. Cette fonction peut soit retourner 
// immédiatement si des données sont déjà disponibles, soit bloquer et mettre le processus en pause 
// (en attendant sur la condition POSIX) jusqu’à ce que ce soit le cas.
int attenteLecteur(struct memPartage *zone){
    pthread_mutex_lock(&zone->header->mutex);
    while(zone->header->etat != ETAT_PRET_AVEC_DONNEES){
        pthread_cond_wait(&zone->header->condLecteur, &zone->header->mutex);
    }
    return 0;
}

// Fonction spéciale similaire à attenteLecteur, mais asynchrone : cette fonction ne bloque jamais.
// Cela est utile pour le compositeur, qui ne doit pas bloquer l'entièreté des flux si un seul est plus lent.
// Utilisez la valeur de retour pour permettre à l'appelant de déterminer si la zone mémoire partagée
// est prête ou non à être lue.
// Lorsque cette fonction retourne une valeur indiquant que la lecture est possible, le mutex devrait être 
// verrouillé par le processus en cours!
// return 0 si prêt, -1 sinon
int attenteLecteurAsync(struct memPartage *zone){
    if(pthread_mutex_trylock(&zone->header->mutex) != 0){ // trylock pour ne pas bloquer si le mutex est déjà verrouillé
        return -1; // Mutex déjà verrouillé, pas disponible
    }
    if(zone->header->etat != ETAT_PRET_AVEC_DONNEES){
        pthread_mutex_unlock(&zone->header->mutex);
        return -1; // Pas prêt
    }
    return 0; // Prêt et mutex verrouillé
}

// Appelée par l'écrivain pour se mettre en attente de la lecture du résultat précédent par un lecteur
// Lorsque cette fonction retourne, le mutex devrait être verrouillé par le processus en cours!
int attenteEcrivain(struct memPartage *zone){
    pthread_mutex_lock(&zone->header->mutex);
    while(zone->header->etat == ETAT_PRET_AVEC_DONNEES){
        pthread_cond_wait(&zone->header->condEcrivain, &zone->header->mutex);
    }
    return 0;
}

// Appelée par le lecteur pour signaler qu'il a fini de lire (réveille l'écrivain correspondant)
// IMPORTANT: Cette fonction assume que le mutex est déjà verrouillé par l'appelant
void signalLecteur(struct memPartage *zone){
    zone->header->etat = ETAT_PRET_SANS_DONNEES;
    pthread_cond_signal(&zone->header->condEcrivain);
    pthread_mutex_unlock(&zone->header->mutex);
}

// Appelée par l'écrivain pour signaler qu'il a fini d'écrire (réveille le lecteur correspondant)
// IMPORTANT: Cette fonction assume que le mutex est déjà verrouillé par l'appelant
void signalEcrivain(struct memPartage *zone){
    zone->header->etat = ETAT_PRET_AVEC_DONNEES;
    pthread_cond_signal(&zone->header->condLecteur);
    pthread_mutex_unlock(&zone->header->mutex);
}
