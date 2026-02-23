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
#include <stddef.h>

// Variables globales pour gérer les pools de mémoire
static void*  poolGros = NULL;        // Pool pour les gros blocs (images)
static void*  poolPetits = NULL;      // Pool pour les petits blocs (autres allocations)
static size_t tailleGrosBloc = 0;

// États des gros blocs : 0 = libre, >0 = début d'allocation (nombre de blocs contigus), -1 = suite d'allocation
static int grosBlocsLibres[ALLOC_N_GROS_BLOCS] = {0};
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
    if (taille == 0) {
        return NULL;
    }

    if (taille > ALLOC_TAILLE_PETIT) {

        if (tailleGrosBloc == 0 || poolGros == NULL) {
            return NULL;
        }

        // Nombre de gros blocs nécessaires pour satisfaire la demande
        size_t nBlocs = (taille + tailleGrosBloc - 1) / tailleGrosBloc;
        if (nBlocs == 0 || nBlocs > ALLOC_N_GROS_BLOCS) {
            return NULL;
        }

        for (int count = 0; count < ALLOC_N_GROS_BLOCS; count++) {
            int debut = (dernierGrosLibre + count) % ALLOC_N_GROS_BLOCS;

            // Vérifier si nBlocs contigus sont libres (avec bouclage circulaire)
            int peutAllouer = 1;
            for (size_t k = 0; k < nBlocs; ++k) {
                int idx = (debut + (int)k) % ALLOC_N_GROS_BLOCS;
                if (grosBlocsLibres[idx] != 0) {
                    peutAllouer = 0;
                    break;
                }
            }

            if (peutAllouer) {
                grosBlocsLibres[debut] = (int)nBlocs;
                for (size_t k = 1; k < nBlocs; ++k) {
                    int idx = (debut + (int)k) % ALLOC_N_GROS_BLOCS;
                    grosBlocsLibres[idx] = -1;
                }
                dernierGrosLibre = (debut + (int)nBlocs) % ALLOC_N_GROS_BLOCS;

                char* base = (char*)poolGros;
                return base + (size_t)debut * tailleGrosBloc;
            }
        }
    }
    else {

        if (poolPetits == NULL) {
            return NULL;
        }

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

    if (poolGros == NULL || poolPetits == NULL)
        return;

    char* p = (char*)ptr;

    char* debutGros  = (char*)poolGros;
    char* finGros    = debutGros + ALLOC_N_GROS_BLOCS * tailleGrosBloc;

    char* debutPetit = (char*)poolPetits;
    char* finPetit   = debutPetit + ALLOC_N_PETITS_BLOCS * ALLOC_TAILLE_PETIT;

    // Vérifie pool gros blocs
    if (p >= debutGros && p < finGros) {

        if (tailleGrosBloc == 0)
            return;

        ptrdiff_t delta = p - debutGros;
        if ((size_t)delta % tailleGrosBloc != 0)
            return;

        int index = (int)((p - debutGros) / tailleGrosBloc);

        if (index >= 0 && index < ALLOC_N_GROS_BLOCS) {
            int nBlocs = grosBlocsLibres[index];

            // Si jamais on reçoit un pointeur sur un bloc de continuation,
            // on retrouve le début de l'allocation.
            if (nBlocs == -1) {
                int j = index;
                for (int steps = 0; steps < ALLOC_N_GROS_BLOCS; ++steps) {
                    j = (j - 1 + ALLOC_N_GROS_BLOCS) % ALLOC_N_GROS_BLOCS;
                    if (grosBlocsLibres[j] > 0) {
                        index = j;
                        nBlocs = grosBlocsLibres[j];
                        break;
                    }
                    if (grosBlocsLibres[j] == 0) {
                        return;
                    }
                }
            }

            if (nBlocs > 0) {
                for (int k = 0; k < nBlocs; ++k) {
                    int idx = (index + k) % ALLOC_N_GROS_BLOCS;
                    grosBlocsLibres[idx] = 0;
                }
                dernierGrosLibre = index;
            }
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