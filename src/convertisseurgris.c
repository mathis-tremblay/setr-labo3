/******************************************************************************
 * Laboratoire 3
 * GIF-3004 Systèmes embarqués temps réel
 * Hiver 2026
 * Marc-André Gardner
 * 
 * Fichier implémentant le programme de conversion en niveaux de gris
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
    
    // Code lisant les options sur la ligne de commande
    char *entree, *sortie;                          // Zones memoires d'entree et de sortie
    struct SchedParams schedParams = {0};           // Paramètres de l'ordonnanceur

    if(argc < 2){
        printf("Nombre d'arguments insuffisant\n");
        return -1;
    }

    if(strcmp(argv[1], "--debug") == 0){
        // Mode debug, vous pouvez changer ces valeurs pour ce qui convient dans vos tests
        printf("Mode debug selectionne pour le convertisseur niveau de gris\n");
        entree = (char*)"/mem1";
        sortie = (char*)"/mem2";
    }
    else{
        int c;
        opterr = 0;

        while ((c = getopt (argc, argv, "s:d:")) != -1){
            switch (c) {
                case 's':
                    parseSchedOption(optarg, &schedParams);
                    break;
                case 'd':
                    parseDeadlineParams(optarg, &schedParams);
                    break;
                default:
                    continue;
            }
        }

        // Ce qui suit est la description des zones memoires d'entree et de sortie
        if(argc - optind < 2){
            printf("Arguments manquants (fichier_entree flux_sortie)\n");
            return -1;
        }
        entree = argv[optind];
        sortie = argv[optind+1];
    }

    printf("Initialisation convertisseur, entree=%s, sortie=%s, mode d'ordonnancement=%i\n", entree, sortie, schedParams.modeOrdonnanceur);
    
    // Changement de mode d'ordonnancement
    appliquerOrdonnancement(&schedParams, "convertisseur");
    
    // TODO : Écrivez ici le code initialisant les zones mémoire partagées (une en entrée, en tant que lecteur, et l'autre en sortie,
    // en tant qu'écrivain).
    // Initialisez également votre allocateur mémoire (avec prepareMemoire). Assurez-vous que toute la mémoire utilisée dans la
    // section critique est ainsi préallouée ET bloquée (voir documentation de mlock/mlockall).

    // Zone d'entrée (lecteur)
    struct memPartage zoneEntree;
    if(initMemoirePartageeLecteur(entree, &zoneEntree) != 0){
        fprintf(stderr, "Erreur initMemoirePartageeLecteur(%s)\n", entree);
        return -1;
    }

    // Préparer les infos de sortie (même dimensions/fps, mais 1 canal)
    struct videoInfos infosOut = zoneEntree.header->infos;
    infosOut.canaux = 1;

    // Zone de sortie (écrivain)
    struct memPartage zoneSortie;
    if(initMemoirePartageeEcrivain(sortie, &zoneSortie, &infosOut) != 0){
        fprintf(stderr, "Erreur initMemoirePartageeEcrivain(%s)\n", sortie);
        return -1;
    }

    // Taille des images
    const size_t tailleIn  = (size_t)zoneEntree.header->infos.largeur *
                             (size_t)zoneEntree.header->infos.hauteur *
                             (size_t)zoneEntree.header->infos.canaux;
    const size_t tailleOut = (size_t)infosOut.largeur *
                             (size_t)infosOut.hauteur *
                             (size_t)infosOut.canaux;

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
    
    // Section critique (boucle à l'infini).
    while(1){
        // Écrivez le code permettant de convertir une image en niveaux de gris, en utilisant la
        // fonction convertToGray de utils.c. Votre code doit lire une image depuis une zone mémoire 
        // partagée et envoyer le résultat sur une autre zone mémoire partagée.

        // Attendre une trame en entrée, copier localement, relâcher le producteur
        evenementProfilage(&profInfos, ETAT_ATTENTE_MUTEXLECTURE);
        attenteLecteur(&zoneEntree);

        memcpy(bufIn, zoneEntree.data, tailleIn);
        signalLecteur(&zoneEntree);

        // Traitement (hors mutex)
        evenementProfilage(&profInfos, ETAT_TRAITEMENT);
        if(zoneEntree.header->infos.canaux == 1){
            memcpy(bufOut, bufIn, tailleOut);
        } else {
            convertToGray(bufIn,
                          zoneEntree.header->infos.hauteur,
                          zoneEntree.header->infos.largeur,
                          zoneEntree.header->infos.canaux,
                          bufOut);
        }

        // Attendre que la sortie soit prête, écrire, puis signaler le lecteur aval
        evenementProfilage(&profInfos, ETAT_ATTENTE_MUTEXECRITURE);
        attenteEcrivain(&zoneSortie);

        memcpy(zoneSortie.data, bufOut, tailleOut);
        signalEcrivain(&zoneSortie);
    }

    return 0;
}