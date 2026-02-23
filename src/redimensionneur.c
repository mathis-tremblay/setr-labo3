/******************************************************************************
 * Laboratoire 3
 * GIF-3004 Systèmes embarqués temps réel
 * Hiver 2026
 * Marc-André Gardner
 * 
 * Fichier implémentant le programme de redimensionnement d'images
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
    
    
    // Écrivez le code permettant de redimensionner une image (en utilisant les fonctions précodées
    // dans utils.c, celles commençant par "resize"). Votre code doit lire une image depuis une zone 
    // mémoire partagée et envoyer le résultat sur une autre zone mémoire partagée.
    // N'oubliez pas de respecter la syntaxe de la ligne de commande présentée dans l'énoncé.

    // ---------------------------------------------------------------------
    // Lecture des options sur la ligne de commande
    // ---------------------------------------------------------------------
    char *entree, *sortie;                          // Zones memoires d'entree et de sortie
    struct SchedParams schedParams = {0};           // Paramètres de l'ordonnanceur

    // Paramètres spécifiques au redimensionneur
    unsigned int out_w = 0;
    unsigned int out_h = 0;
    unsigned int mode_resize = 0; // 0 = nearest neighbor, 1 = bilinear

    if(argc < 2){
        printf("Nombre d'arguments insuffisant\n");
        return -1;
    }

    if(strcmp(argv[1], "--debug") == 0){
        // Mode debug, vous pouvez changer ces valeurs pour ce qui convient dans vos tests
        printf("Mode debug selectionne pour le redimensionneur\n");
        entree = (char*)"/mem1";
        sortie = (char*)"/mem2";
        out_w = 427;
        out_h = 240;
        mode_resize = 0;
    }
    else{
        int c;
        opterr = 0;

        while ((c = getopt (argc, argv, "s:d:w:h:r:")) != -1){
            switch (c) {
                case 's':
                    parseSchedOption(optarg, &schedParams);
                    break;
                case 'd':
                    parseDeadlineParams(optarg, &schedParams);
                    break;
                case 'w':
                    out_w = (unsigned int)atoi(optarg);
                    break;
                case 'h':
                    out_h = (unsigned int)atoi(optarg);
                    break;
                case 'r':
                    mode_resize = (unsigned int)atoi(optarg);
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

    printf("Initialisation redimensionneur, entree=%s, sortie=%s, mode d'ordonnancement=%i, w=%u, h=%u, r=%u\n",
            entree, sortie, schedParams.modeOrdonnanceur, out_w, out_h, mode_resize);

    // Changement de mode d'ordonnancement
    appliquerOrdonnancement(&schedParams, "redimensionneur");

    // ---------------------------------------------------------------------
    // Initialisation des zones mémoire partagées + allocateur temps réel
    // ---------------------------------------------------------------------

    // Zone d'entrée (lecteur)
    struct memPartage zoneEntree;
    if(initMemoirePartageeLecteur(entree, &zoneEntree) != 0){
        fprintf(stderr, "Erreur initMemoirePartageeLecteur(%s)\n", entree);
        return -1;
    }

    // Préparer les infos de sortie (nouvelle largeur/hauteur, mêmes canaux/fps)
    struct videoInfos infosOut = zoneEntree.header->infos;
    infosOut.largeur = out_w;
    infosOut.hauteur = out_h;

    // Zone de sortie (écrivain)
    struct memPartage zoneSortie;
    if(initMemoirePartageeEcrivain(sortie, &zoneSortie, &infosOut) != 0){
        fprintf(stderr, "Erreur initMemoirePartageeEcrivain(%s)\n", sortie);
        return -1;
    }

    // Taille des images
    const unsigned int in_w = zoneEntree.header->infos.largeur;
    const unsigned int in_h = zoneEntree.header->infos.hauteur;
    const unsigned int n_channels = zoneEntree.header->infos.canaux;

    const size_t tailleIn  = (size_t)in_w * (size_t)in_h * (size_t)n_channels;
    const size_t tailleOut = (size_t)out_w * (size_t)out_h * (size_t)n_channels;

    // Initialiser l'allocateur mémoire temps réel (pool + mlock des pools)
    if(prepareMemoire(tailleIn, tailleOut) != 0){
        fprintf(stderr, "Erreur prepareMemoire(tailleIn=%zu, tailleOut=%zu)\n", tailleIn, tailleOut);
        return -1;
    }

    // Verrouiller les pages mémoire du processus (peut nécessiter sudo)
    if(mlockall(MCL_CURRENT | MCL_FUTURE) != 0){
        perror("mlockall (avertissement)");
    }

    // Pré-calcul des grilles de redimensionnement (commun à toutes les images)
    ResizeGrid rg;
    if(mode_resize == 0){
        rg = resizeNearestNeighborInit(out_h, out_w, in_h, in_w);
    } else {
        rg = resizeBilinearInit(out_h, out_w, in_h, in_w);
    }

    // Buffers de travail
    unsigned char *bufIn  = (unsigned char*)tempsreel_malloc(tailleIn);
    unsigned char *bufOut = (unsigned char*)tempsreel_malloc(tailleOut);
    if(bufIn == NULL || bufOut == NULL){
        fprintf(stderr, "Erreur tempsreel_malloc pour buffers de travail (bufIn=%p, bufOut=%p)\n",
                (void*)bufIn, (void*)bufOut);
        return -1;
    }

    // ---------------------------------------------------------------------
    // Section critique (boucle à l'infini)
    // ---------------------------------------------------------------------
    while(1){
        evenementProfilage(&profInfos, ETAT_ATTENTE_MUTEXLECTURE);
        attenteLecteur(&zoneEntree);

        memcpy(bufIn, zoneEntree.data, tailleIn);
        signalLecteur(&zoneEntree);

        evenementProfilage(&profInfos, ETAT_TRAITEMENT);
        if(mode_resize == 0){
            resizeNearestNeighbor(bufIn, in_h, in_w,
                                  bufOut, out_h, out_w,
                                  rg, n_channels);
        } else {
            resizeBilinear(bufIn, in_h, in_w,
                           bufOut, out_h, out_w,
                           rg, n_channels);
        }

        evenementProfilage(&profInfos, ETAT_ATTENTE_MUTEXECRITURE);
        attenteEcrivain(&zoneSortie);

        memcpy(zoneSortie.data, bufOut, tailleOut);
        signalEcrivain(&zoneSortie);
    }

    return 0;
}