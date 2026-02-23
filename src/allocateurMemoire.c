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
#include <stdio.h>

// Variables globales pour gérer les pools de mémoire
static void*  poolGros = NULL;        // Pool pour les gros blocs (images)
static void*  poolPetits = NULL;      // Pool pour les petits blocs (autres allocations)
static size_t tailleGrosBloc = 0;

static int grosBlocsLibres[ALLOC_N_GROS_BLOCS] = {0};      // 0 = libre, 1 = utilisé
static int petitsBlocsLibres[ALLOC_N_PETITS_BLOCS] = {0};

static int dernierGrosLibre = 0;      // Index du dernier bloc gros libéré
static int dernierPetitLibre = 0;     // Index du dernier bloc petit libéré


// Prépare les buffers nécessaires pour une allocation correspondante aux tailles
// d'images passées en paramètre. Retourne 0 en cas de succès, et -1 si un
// problème (par exemple manque de mémoire) est survenu.
int prepareMemoire(size_t tailleImageEntree, size_t tailleImageSortie)
{
    struct rlimit rlim;

    tailleGrosBloc = (tailleImageEntree > tailleImageSortie) ?
                      tailleImageEntree : tailleImageSortie;

    size_t taillePoolGros   = ALLOC_N_GROS_BLOCS   * tailleGrosBloc;
    size_t taillePoolPetits = ALLOC_N_PETITS_BLOCS * ALLOC_TAILLE_PETIT;
    size_t tailleTotal      = taillePoolGros + taillePoolPetits;

    // 1. Augmenter la limite de mémoire verrouillable
    rlim.rlim_cur = tailleTotal + 1024 * 1024;
    rlim.rlim_max = tailleTotal + 1024 * 1024;

    if (setrlimit(RLIMIT_MEMLOCK, &rlim) != 0) {
        perror("setrlimit");
        // Peut échouer sans root — on continue quand même
    }

    // 2. Allocation du pool gros blocs
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

    // 3. Allocation du pool petits blocs
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

    // 4. Initialiser les blocs comme libres
    memset(grosBlocsLibres,   0, sizeof(grosBlocsLibres));
    memset(petitsBlocsLibres, 0, sizeof(petitsBlocsLibres));

    return 0;
}


// Ces deux fonctions doivent pouvoir s'utiliser exactement comme malloc() et free()
// (dans la limite de la mémoire disponible, bien sûr)

void* tempsreel_malloc(size_t taille)
{
    if (taille > ALLOC_TAILLE_PETIT) {

        for (int count = 0; count < ALLOC_N_GROS_BLOCS; count++) {
            int i = (dernierGrosLibre + count) % ALLOC_N_GROS_BLOCS;

            if (grosBlocsLibres[i] == 0) {
                grosBlocsLibres[i] = 1;
                dernierGrosLibre = (i + 1) % ALLOC_N_GROS_BLOCS;

                char* base = (char*)poolGros;
                return base + (size_t)i * tailleGrosBloc;
            }
        }
    }
    else {

        for (int count = 0; count < ALLOC_N_PETITS_BLOCS; count++) {
            int i = (dernierPetitLibre + count) % ALLOC_N_PETITS_BLOCS;

            if (petitsBlocsLibres[i] == 0) {
                petitsBlocsLibres[i] = 1;
                dernierPetitLibre = (i + 1) % ALLOC_N_PETITS_BLOCS;

                char* base = (char*)poolPetits;
                return base + (size_t)i * ALLOC_TAILLE_PETIT;
            }
        }
    }

    return NULL;
}


// Marque le bloc comme libre en fonction de son adresse.
// Ne fait rien si le pointeur ne correspond à aucun bloc alloué.
void tempsreel_free(void* ptr)
{
    if (ptr == NULL)
        return;

    char* p = (char*)ptr;

    char* debutGros  = (char*)poolGros;
    char* finGros    = debutGros + ALLOC_N_GROS_BLOCS * tailleGrosBloc;

    char* debutPetit = (char*)poolPetits;
    char* finPetit   = debutPetit + ALLOC_N_PETITS_BLOCS * ALLOC_TAILLE_PETIT;

    // Vérifie pool gros blocs
    if (p >= debutGros && p < finGros) {

        int index = (int)((p - debutGros) / tailleGrosBloc);

        if (index >= 0 && index < ALLOC_N_GROS_BLOCS) {
            grosBlocsLibres[index] = 0;
            dernierGrosLibre = index;
        }
    }
    // Vérifie pool petits blocs
    else if (p >= debutPetit && p < finPetit) {

        int index = (int)((p - debutPetit) / ALLOC_TAILLE_PETIT);

        if (index >= 0 && index < ALLOC_N_PETITS_BLOCS) {
            petitsBlocsLibres[index] = 0;
            dernierPetitLibre = index;
        }
    }
}