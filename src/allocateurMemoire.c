/******************************************************************************
 * Laboratoire 3
 * GIF-3004 Systèmes embarqués temps réel
 * Hiver 2026
 * Marc-André Gardner
 * 
 * Fichier implémentant les fonctions de l'allocateur mémoire temps réel
 ******************************************************************************/

#include "allocateurMemoire.h"
#include <sys/resource.h>
#include <sys/mman.h>
#include <string.h>

// Variables globales pour gérer les pools de mémoire
static void* poolGros = NULL; // Pool pour les gros blocs (images)
static void* poolPetits = NULL; // Pool pour les petits blocs (autres allocations)
static size_t tailleGrosBloc = 0;
static int grosBlocsLibres[ALLOC_N_GROS_BLOCS] = {0}; // 0 = libre, 1 = utilisé
static int petitsBlocsLibres[ALLOC_N_PETITS_BLOCS] = {0};

// Prépare les buffers nécessaires pour une allocation correspondante aux tailles
// d'images passées en paramètre. Retourne 0 en cas de succès, et -1 si un
// problème (par exemple manque de mémoire) est survenu.
int prepareMemoire(size_t tailleImageEntree, size_t tailleImageSortie){
    // 1. Augmenter la limite de mémoire verrouillable avec setrlimit
    struct rlimit rlim;
    tailleGrosBloc = (tailleImageEntree > tailleImageSortie) ? tailleImageEntree : tailleImageSortie;
    
    size_t taillePoolGros = ALLOC_N_GROS_BLOCS * tailleGrosBloc;
    size_t taillePoolPetits = ALLOC_N_PETITS_BLOCS * ALLOC_TAILLE_PETIT;
    size_t tailleTotal = taillePoolGros + taillePoolPetits;
    
    rlim.rlim_cur = tailleTotal + 1024*1024; // Ajouter 1 Mo de marge
    rlim.rlim_max = tailleTotal + 1024*1024;
    
    if (setrlimit(RLIMIT_MEMLOCK, &rlim) != 0) {
        perror("setrlimit");
        // Note: peut échouer si pas root, mais on essaie quand même mlock après
    }
    
    // 2. Allouer le pool pour les gros blocs
    poolGros = malloc(taillePoolGros);
    if (poolGros == NULL) {
        perror("malloc poolGros");
        return -1;
    }
    
    if (mlock(poolGros, taillePoolGros) != 0) {
        perror("mlock poolGros");
        free(poolGros);
        poolGros = NULL;
        return -1;
    }
    
    // 3. Allouer le pool pour les petits blocs
    poolPetits = malloc(taillePoolPetits);
    if (poolPetits == NULL) {
        perror("malloc poolPetits");
        munlock(poolGros, taillePoolGros);
        free(poolGros);
        poolGros = NULL;
        return -1;
    }
    
    if (mlock(poolPetits, taillePoolPetits) != 0) {
        perror("mlock poolPetits");
        munlock(poolGros, taillePoolGros);
        free(poolGros);
        free(poolPetits);
        poolGros = NULL;
        poolPetits = NULL;
        return -1;
    }
    
    // 4. Initialiser tous les blocs comme libres
    memset(grosBlocsLibres, 0, sizeof(grosBlocsLibres));
    memset(petitsBlocsLibres, 0, sizeof(petitsBlocsLibres));
    
    return 0;
}

// Ces deux fonctions doivent pouvoir s'utiliser exactement comme malloc() et free()
// (dans la limite de la mémoire disponible, bien sûr)

// Trouve un bloc libre dans le pool spécifié et le marque comme utilisé. Retourne un pointeur vers ce bloc, ou NULL si aucun bloc n'est disponible.
void* tempsreel_malloc(size_t taille){
    if (taille > ALLOC_TAILLE_PETIT) {
        // Allocation dans le pool des gros blocs
        for (int i = 0; i < ALLOC_N_GROS_BLOCS; i++) {
            if (grosBlocsLibres[i] == 0) {
                grosBlocsLibres[i] = 1; // Marquer comme utilisé
                return (char*)poolGros + i * tailleGrosBloc;
            }
        }
    } else {
        // Allocation dans le pool des petits blocs
        for (int i = 0; i < ALLOC_N_PETITS_BLOCS; i++) {
            if (petitsBlocsLibres[i] == 0) {
                petitsBlocsLibres[i] = 1; // Marquer comme utilisé
                return (char*)poolPetits + i * ALLOC_TAILLE_PETIT;
            }
        }
    }
    return NULL;
}

// Marque le bloc comme libre en fonction de son adresse. Ne fait rien si le pointeur ne correspond à aucun bloc alloué.
void tempsreel_free(void* ptr){
    // verifie si adresse correspond au pool des gros blocs
    if (ptr >= poolGros && ptr < (char*)poolGros + ALLOC_N_GROS_BLOCS * tailleGrosBloc /*max*/) { // note : (char*) cast le ptr pour faire des calculs d'adresses en octets
        // Libération d'un bloc du pool des gros blocs
        int index = ((char*)ptr - (char*)poolGros) / tailleGrosBloc;
        if (index >= 0 && index < ALLOC_N_GROS_BLOCS) {
            grosBlocsLibres[index] = 0; // Marquer comme libre
        }
    } else if (ptr >= poolPetits && ptr < (char*)poolPetits + ALLOC_N_PETITS_BLOCS * ALLOC_TAILLE_PETIT /*max*/) {
        // Libération d'un bloc du pool des petits blocs
        int index = ((char*)ptr - (char*)poolPetits) / ALLOC_TAILLE_PETIT;
        if (index >= 0 && index < ALLOC_N_PETITS_BLOCS) {
            petitsBlocsLibres[index] = 0; // Marquer comme libre
        }
    }
}

