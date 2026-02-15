/******************************************************************************
 * Laboratoire 3
 * GIF-3004 Systèmes embarqués temps réel
 * Hiver 2026
 * Marc-André Gardner
 * 
 * Fichier implémentant le programme de décodage des fichiers ULV
 ******************************************************************************/


// Gestion des ressources et permissions
#include <sys/resource.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <getopt.h>
#include <string.h>
#include <stdint.h>

#include "allocateurMemoire.h"
#include "commMemoirePartagee.h"
#include "utils.h"

#include "jpgd.h"

// Définition de diverses structures pouvant vous être utiles pour la lecture d'un fichier ULV
#define HEADER_SIZE 4
const char header[] = "SETR";

/*struct videoInfos{
        uint32_t largeur;
        uint32_t hauteur;
        uint32_t canaux;
        uint32_t fps;
};*/

/******************************************************************************
* FORMAT DU FICHIER VIDEO
* Offset     Taille     Type      Description
* 0          4          char      Header (toujours "SETR" en ASCII)
* 4          4          uint32    Largeur des images du vidéo
* 8          4          uint32    Hauteur des images du vidéo
* 12         4          uint32    Nombre de canaux dans les images
* 16         4          uint32    Nombre d'images par seconde (FPS)
* 20         4          uint32    Taille (en octets) de la première image -> N
* 24         N          char      Contenu de la première image (row-first)
* 24+N       4          uint32    Taille (en octets) de la seconde image -> N2
* 24+N+4     N2         char      Contenu de la seconde image
* 24+N+N2    4          uint32    Taille (en octets) de la troisième image -> N2
* ...                             Toutes les images composant la vidéo, à la suite
*            4          uint32    0 (indique la fin du fichier)
******************************************************************************/


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
    char *entree, *sortie;                          // Fichier d'entree et zone memoire de sortie
    struct SchedParams schedParams = {0,0,0,0};           // Paramètres de l'ordonnanceur

    if(argc < 2){
        printf("Nombre d'arguments insuffisant\n");
        return -1;
    }

    if(strcmp(argv[1], "--debug") == 0){
        // Mode debug, vous pouvez changer ces valeurs pour ce qui convient dans vos tests
        printf("Mode debug selectionne pour le decodeur\n");
        entree = (char*)"video1.ulv";
        sortie = (char*)"/mem1";
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

        // Ce qui suit est le path du fichier d'entree et le flux memoire de sortie
        if(argc - optind < 2){
            printf("Arguments manquants (fichier_entree flux_sortie)\n");
            return -1;
        }
        entree = argv[optind];
        sortie = argv[optind+1];
    }

    printf("Initialisation decodeur, entree=%s, sortie=%s, mode d'ordonnancement=%i\n", entree, sortie, schedParams.modeOrdonnanceur);
    
    // Changement de mode d'ordonnancement
    appliquerOrdonnancement(&schedParams, "decodeur");
    
    // Écrivez le code de décodage depuis un fichier et d'envoi sur la zone mémoire partagée ici!
    // N'oubliez pas que vous pouvez utiliser jpgd::decompress_jpeg_image_from_memory()
    // pour décoder une image JPEG contenue dans un buffer!
    // N'oubliez pas également que ce décodeur doit lire les fichiers ULV EN BOUCLE
    
    // Ouvrir et mapper le fichier ULV en RAM
    int fd = open(entree, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "Impossible d'ouvrir le fichier d'entree: %s\n", strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)(HEADER_SIZE + 4 * sizeof(uint32_t))) {
        fprintf(stderr, "Fichier d'entree non conforme (taille insuffisante)\n");
        close(fd);
        return -1;
    }

    unsigned char *fileMap = (unsigned char*)mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0);
    if (fileMap == MAP_FAILED) {
        fprintf(stderr, "mmap echoue: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    unsigned char *cursor = fileMap;

    // lire header
    if (memcmp(cursor, header, HEADER_SIZE) != 0) {
        fprintf(stderr, "Fichier d'entree non conforme (header incorrect)\n");
        munmap(fileMap, st.st_size);
        close(fd);
        return -1;
    }
    cursor += HEADER_SIZE;

    // lire infos video
    struct videoInfos infos;
    memcpy(&infos.largeur, cursor, sizeof(uint32_t));
    cursor += sizeof(uint32_t);
    memcpy(&infos.hauteur, cursor, sizeof(uint32_t));
    cursor += sizeof(uint32_t);
    memcpy(&infos.canaux, cursor, sizeof(uint32_t));
    cursor += sizeof(uint32_t);
    memcpy(&infos.fps, cursor, sizeof(uint32_t));
    cursor += sizeof(uint32_t);

    // Initialiser la zone mémoire partagée de sortie
    struct memPartage zoneSortie;
    if (initMemoirePartageeEcrivain(sortie, &zoneSortie, &infos) != 0) {
        fprintf(stderr, "Erreur d'initialisation de la memoire partagee de sortie\n");
        munmap(fileMap, st.st_size);
        close(fd);
        return -1;
    }

    // Préparer l'allocateur mémoire temps réel (décodeur: pas d'entrée, seulement la taille de sortie)
    size_t tailleImage = (size_t)infos.largeur * (size_t)infos.hauteur * (size_t)infos.canaux;
    if (prepareMemoire(tailleImage, tailleImage) != 0) {
        fprintf(stderr, "Erreur d'initialisation de l'allocateur memoire temps reel\n");
        munmap(fileMap, st.st_size);
        close(fd);
        return -1;
    }

    // Cadence cible en microsecondes (si fps valide)
    uint32_t delaiUsec = (infos.fps > 0) ? (1000000U / infos.fps) : 0U;

    // Lire et décoder les images en boucle
    while (1) {
        if (cursor + sizeof(uint32_t) > fileMap + st.st_size) {
            fprintf(stderr, "Fichier d'entree non conforme (taille insuffisante pour lire la taille de l'image)\n");
            break;
        }

        uint32_t imageSize;
        memcpy(&imageSize, cursor, sizeof(uint32_t));
        cursor += sizeof(uint32_t);

        if (imageSize == 0) {
            // Fin du fichier, recommencer depuis le début (après l'en-tête)
            cursor = fileMap + HEADER_SIZE + 4 * sizeof(uint32_t);
            continue;
        }

        if (cursor + imageSize > fileMap + st.st_size) {
            fprintf(stderr, "Fichier d'entree non conforme (taille insuffisante pour lire l'image)\n");
            break;
        }

        unsigned char *imageData = cursor;
        cursor += imageSize;

        int width = (int)infos.largeur;
        int height = (int)infos.hauteur;
        int channels = (int)infos.canaux;
        unsigned char *decodedImage = jpgd::decompress_jpeg_image_from_memory(imageData, imageSize, &width, &height, &channels, (int)infos.canaux);

        if (decodedImage == NULL) {
            fprintf(stderr, "Erreur de decodage JPEG\n");
            continue;
        }

        if ((uint32_t)width != infos.largeur || (uint32_t)height != infos.hauteur || (uint32_t)channels != infos.canaux) {
            fprintf(stderr, "Image decodee invalide (dimension ou canaux inattendus)\n");
            tempsreel_free(decodedImage);
            continue;
        }

        // Écrire l'image décodée dans la zone mémoire partagée (synchro écrivain)
        evenementProfilage(&profInfos, ETAT_ATTENTE_MUTEXECRITURE);
        attenteEcrivain(&zoneSortie);
        evenementProfilage(&profInfos, ETAT_TRAITEMENT);
        memcpy(zoneSortie.data, decodedImage, zoneSortie.tailleDonnees);
        signalEcrivain(&zoneSortie);

        tempsreel_free(decodedImage);

        // Libérer le processeur pour respecter la cadence cible
        if (delaiUsec > 0) {
            evenementProfilage(&profInfos, ETAT_ENPAUSE);
            usleep(delaiUsec);
        }
    }
    munmap(zoneSortie.header, sizeof(struct memPartageHeader) + zoneSortie.tailleDonnees);
    close(zoneSortie.fd);
    shm_unlink(sortie);  // Supprime /dev/shm/mem1
    return 0;
}
