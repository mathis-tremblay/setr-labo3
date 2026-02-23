/******************************************************************************
 * Laboratoire 3
 * GIF-3004 Systèmes embarqués temps réel
 * Hiver 2026
 * Marc-André Gardner
 * 
 * Fichier implémentant le programme de filtrage des images
 ******************************************************************************/

// Gestion des ressources et permissions
#include <sys/resource.h>

#include <getopt.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "allocateurMemoire.h"
#include "commMemoirePartagee.h"
#include "utils.h"


int main(int argc, char* argv[]){
    // On desactive le buffering pour les printf(), pour qu'il soit possible de les voir depuis votre ordinateur
    setbuf(stdout, NULL);
    
    // Initialise le profilage
    char signatureProfilage[128] = {0};
    char* nomProgramme = (argv[0][0] == '.') ? argv[0]+2 : argv[0];
    snprintf(signatureProfilage, 128, "profilage-%s-%u.txt", nomProgramme, (unsigned int)getpid());
    InfosProfilage profInfos;
    initProfilage(&profInfos, signatureProfilage);
    
    // Premier evenement de profilage : l'initialisation du programme
    evenementProfilage(&profInfos, ETAT_INITIALISATION);
    
    // Écrivez le code permettant de filtrer une image (en utilisant les fonctions précodées
    // dans utils.c). Votre code doit lire une image depuis une zone mémoire partagée et
    // envoyer le résultat sur une autre zone mémoire partagée.
    // N'oubliez pas de respecter la syntaxe de la ligne de commande présentée dans l'énoncé.

    // ---------------------------------------------------------------------
    // Lecture des options sur la ligne de commande
    // ---------------------------------------------------------------------
    char *entree, *sortie;                          // Zones memoires d'entree et de sortie
    struct SchedParams schedParams = {0};           // Paramètres de l'ordonnanceur

    unsigned int typeFiltre = 0;                    // -f : 0 = passe-bas, 1 = passe-haut

    if(argc < 2){
        printf("Nombre d'arguments insuffisant\n");
        return -1;
    }

    if(strcmp(argv[1], "--debug") == 0){
        // Mode debug, vous pouvez changer ces valeurs pour ce qui convient dans vos tests
        printf("Mode debug selectionne pour le filtreur\n");
        entree = (char*)"/mem1";
        sortie = (char*)"/mem2";
        typeFiltre = 0;
    }
    else{
        int c;
        opterr = 0;

        while ((c = getopt (argc, argv, "s:d:f:")) != -1){
            switch (c) {
                case 's':
                    parseSchedOption(optarg, &schedParams);
                    break;
                case 'd':
                    parseDeadlineParams(optarg, &schedParams);
                    break;
                case 'f':
                    typeFiltre = (unsigned int)atoi(optarg);
                    break;
                default:
                    continue;
            }
        }

        // Ce qui suit est la description des zones memoires d'entree et de sortie
        if(argc - optind < 2){
            printf("Arguments manquants (flux_entree flux_sortie)\n");
            return -1;
        }
        entree = argv[optind];
        sortie = argv[optind+1];
    }

    printf("Initialisation filtreur, entree=%s, sortie=%s, mode d'ordonnancement=%i, filtre=%u\n",
            entree, sortie, schedParams.modeOrdonnanceur, typeFiltre);

    // Changement de mode d'ordonnancement
    appliquerOrdonnancement(&schedParams, "filtreur");

    // ---------------------------------------------------------------------
    // Initialisation des zones mémoire partagées + allocateur temps réel
    // ---------------------------------------------------------------------

    // Zone d'entrée (lecteur)
    struct memPartage zoneEntree;
    if(initMemoirePartageeLecteur(entree, &zoneEntree) != 0){
        fprintf(stderr, "Erreur initMemoirePartageeLecteur(%s)\n", entree);
        return -1;
    }

    // Infos de sortie identiques à l'entrée (mêmes dimensions/canaux/fps)
    struct videoInfos infosOut = zoneEntree.header->infos;

    // Zone de sortie (écrivain)
    struct memPartage zoneSortie;
    if(initMemoirePartageeEcrivain(sortie, &zoneSortie, &infosOut) != 0){
        fprintf(stderr, "Erreur initMemoirePartageeEcrivain(%s)\n", sortie);
        return -1;
    }

    // Taille des images
    const unsigned int w = zoneEntree.header->infos.largeur;
    const unsigned int h = zoneEntree.header->infos.hauteur;
    const unsigned int cch = zoneEntree.header->infos.canaux;

    const size_t tailleIn  = (size_t)w * (size_t)h * (size_t)cch;
    const size_t tailleOut = tailleIn;

    // Initialiser l'allocateur mémoire temps réel (pool + mlock des pools)
    if(prepareMemoire(tailleIn, tailleOut) != 0){
        fprintf(stderr, "Erreur prepareMemoire(tailleIn=%zu, tailleOut=%zu)\n", tailleIn, tailleOut);
        return -1;
    }

    // Verrouiller les pages mémoire du processus (peut nécessiter sudo)
    // Les pools sont déjà mlock() dans prepareMemoire(), ceci fixe aussi la pile/heap courante et future.
    if(mlockall(MCL_CURRENT | MCL_FUTURE) != 0){
        // Ne pas échouer si la limite RLIMIT_MEMLOCK n'est pas suffisante, mais avertir.
        perror("mlockall (avertissement)");
    }

    // Buffers de travail pour éviter de garder les mutex pendant le traitement
    unsigned char *bufIn  = (unsigned char*)tempsreel_malloc(tailleIn);
    unsigned char *bufOut = (unsigned char*)tempsreel_malloc(tailleOut);
    if(bufIn == NULL || bufOut == NULL){
        fprintf(stderr, "Erreur tempsreel_malloc pour buffers de travail (bufIn=%p, bufOut=%p)\n",
                (void*)bufIn, (void*)bufOut);
        return -1;
    }

    // Paramètres de filtre suggérés par utils.h (ex: 3 et 5)
    const unsigned int kernel_size = 3;
    const float sigma = 5.0f;

    // ---------------------------------------------------------------------
    // Section critique (boucle à l'infini)
    // ---------------------------------------------------------------------
    while(1){
        // Attendre une trame en entrée, copier localement, relâcher le producteur
        evenementProfilage(&profInfos, ETAT_ATTENTE_MUTEXLECTURE);
        attenteLecteur(&zoneEntree);

        memcpy(bufIn, zoneEntree.data, tailleIn);
        signalLecteur(&zoneEntree);

        // Traitement (hors mutex)
        evenementProfilage(&profInfos, ETAT_TRAITEMENT);
        if(typeFiltre == 0){
            lowpassFilter(h, w, bufIn, bufOut, kernel_size, sigma, cch);
        } else {
            highpassFilter(h, w, bufIn, bufOut, kernel_size, sigma, cch);
        }

        // Attendre que la sortie soit prête, écrire, puis signaler le lecteur aval
        evenementProfilage(&profInfos, ETAT_ATTENTE_MUTEXECRITURE);
        attenteEcrivain(&zoneSortie);

        memcpy(zoneSortie.data, bufOut, tailleOut);
        signalEcrivain(&zoneSortie);
    }

    return 0;
}