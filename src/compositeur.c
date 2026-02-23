/******************************************************************************
* Laboratoire 3
* GIF-3004 Systèmes embarqués temps réel
* Hiver 2026
* Marc-André Gardner
* 
* Programme compositeur
*
* Récupère plusieurs flux vidéos à partir d'espaces mémoire partagés et les
* affiche directement dans le framebuffer de la carte graphique.
* 
* IMPORTANT : CE CODE ASSUME QUE TOUS LES FLUX QU'IL REÇOIT SONT EN 427x240
* (427 pixels en largeur, 240 en hauteur). TOUTE AUTRE TAILLE ENTRAINERA UN
* COMPORTEMENT INDÉFINI. Les flux peuvent comporter 1 ou 3 canaux. Dans ce
* dernier cas, ils doivent être dans l'ordre BGR et NON RGB.
*
* Le code permettant l'affichage est inspiré de celui présenté sur le blog
* Raspberry Compote (http://raspberrycompote.blogspot.ie/2014/03/low-level-graphics-on-raspberry-pi-part_14.html),
* par J-P Rosti, publié sous la licence CC-BY 3.0.
*
* Merci à Yannick Hold-Geoffroy pour l'aide apportée pour la gestion
* du framebuffer.
******************************************************************************/

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <getopt.h>

#include <sys/ioctl.h>

#include <sys/time.h>
#include <sys/resource.h>

#include <sys/types.h>

// Allocation mémoire, mmap et mlock
#include <sys/mman.h>

// Gestion des ressources et permissions
#include <sys/resource.h>

// Mesure du temps
#include <time.h>

// Obtenir la taille des fichiers
#include <sys/stat.h>

// Contrôle de la console
#include <linux/fb.h>
#include <linux/kd.h>

// Gestion des erreurs
#include <err.h>
#include <errno.h>

#include "allocateurMemoire.h"
#include "commMemoirePartagee.h"
#include "utils.h"


// Fonction permettant de récupérer le temps courant sous forme double
double get_time()
{
    struct timeval t;
    gettimeofday(&t, NULL);
    return (double)t.tv_sec + (double)(t.tv_usec)*1e-6;
}


// Cette fonction écrit l'image dans le framebuffer, à la position demandée. Elle est déjà codée pour vous,
// mais vous devez l'utiliser correctement. En particulier, n'oubliez pas que cette fonction assume que
// TOUTES LES IMAGES QU'ELLE REÇOIT SONT EN 427x240 (1 ou 3 canaux). Cette fonction peut gérer
// l'affichage de 1, 2, 3 ou 4 images sur le même écran, en utilisant la séparation préconisée dans l'énoncé.
// La position (premier argument) doit être un entier inférieur au nombre total d'images à afficher (second argument).
// Le troisième argument est le descripteur de fichier du framebuffer (nommé fbfb dans la fonction main()).
// Le quatrième argument est un pointeur sur le memory map de ce framebuffer (nommé fbd dans la fonction main()).
// Les cinquième et sixième arguments sont la largeur et la hauteur de ce framebuffer.
// Le septième est une structure contenant l'information sur le framebuffer (nommé vinfo dans la fonction main()).
// Le huitième est la longueur effective d'une ligne du framebuffer (en octets), contenue dans finfo.line_length dans la fonction main().
// Le neuvième argument est le buffer contenant l'image à afficher, et les trois derniers arguments ses dimensions.
void ecrireImage(const int position, const int total,
                    int fbfd, unsigned char* fb, size_t largeurFB, size_t hauteurFB, struct fb_var_screeninfo *vinfoPtr, int fbLineLength,
                    const unsigned char *data, size_t hauteurSource, size_t largeurSource, size_t canauxSource){
    static int currentPage = 0;
    static unsigned char* imageGlobale = NULL;
    if(imageGlobale == NULL)
        imageGlobale = (unsigned char*)calloc(fbLineLength*hauteurFB, 1);

    currentPage = (currentPage+1) % 2;
    unsigned char *currentFramebuffer = fb + currentPage * fbLineLength * hauteurFB;

    if(position >= total){
        return;
    }

    const unsigned char *dataTraite = data;
    unsigned char* d = NULL;
    if(canauxSource == 1){
        d = (unsigned char*)tempsreel_malloc(largeurSource*hauteurSource*3);
        unsigned int pos = 0;
        for(unsigned int i=0; i < hauteurSource; ++i){
            for(unsigned int j=0; j < largeurSource; ++j){
                d[pos++] = data[i*largeurSource + j];
                d[pos++] = data[i*largeurSource + j];
                d[pos++] = data[i*largeurSource + j];
            }
        }
        dataTraite = d;
    }


    if(total == 1){
        // Une seule image en plein écran
        for(unsigned int ligne=0; ligne < hauteurSource; ligne++){
            memcpy(currentFramebuffer + ligne * fbLineLength, dataTraite + ligne * largeurSource * 3, largeurFB * 3);
        }
    }
    else if(total == 2){
        // Deux images
        if(position == 0){
            // Image du haut
            for(unsigned int ligne=0; ligne < hauteurSource; ligne++){
                memcpy(imageGlobale + ligne * fbLineLength, dataTraite + ligne * largeurSource * 3, largeurFB * 3);
            }
        }
        else{
            // Image du bas
            for(unsigned int ligne=hauteurSource; ligne < hauteurSource*2; ligne++){
                memcpy(imageGlobale + ligne * fbLineLength, dataTraite + (ligne-hauteurSource) * largeurSource * 3, largeurFB * 3);
            }
        }
    }
    else if(total == 3 || total == 4){
        // 3 ou 4 images
        off_t offsetLigne = 0;
        off_t offsetColonne = 0;
        switch (position) {
            case 0:
                // En haut, à gauche
                break;
            case 1:
                // En haut, à droite
                offsetColonne = largeurSource;
                break;
            case 2:
                // En bas, à gauche
                offsetLigne = hauteurSource;
                break;
            case 3:
                // En bas, à droite
                offsetLigne = hauteurSource;
                offsetColonne = largeurSource;
                break;
        }
        // On copie les données ligne par ligne
        offsetLigne *= fbLineLength;
        offsetColonne *= 3;
        for(unsigned int ligne=0; ligne < hauteurSource; ligne++){
            memcpy(imageGlobale + offsetLigne + offsetColonne, dataTraite + ligne * largeurSource * 3, largeurSource * 3);
            offsetLigne += fbLineLength;
        }
    }

    if(total > 1)
        memcpy(currentFramebuffer, imageGlobale, fbLineLength*hauteurFB);
        
    if(canauxSource == 1)
        tempsreel_free(d);
        
    vinfoPtr->yoffset = currentPage * vinfoPtr->yres;
    vinfoPtr->activate = FB_ACTIVATE_VBL;
    if (ioctl(fbfd, FBIOPAN_DISPLAY, vinfoPtr)) {
        printf("Erreur lors du changement de buffer (double buffering inactif)!\n");
    }
}



int main(int argc, char* argv[])
{
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
    char *flux[4] = {NULL, NULL, NULL, NULL};
    struct SchedParams schedParams = {0, 0, 0, 0};
    int nbrActifs = 0;

    if(argc < 2){
        printf("Nombre d'arguments insuffisant\n");
        return -1;
    }

    if(strcmp(argv[1], "--debug") == 0){
        printf("Mode debug selectionne pour le compositeur\n");
        flux[0] = (char*)"/mem1";
        nbrActifs = 1;
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

        // Les flux d'entrée (de 1 à 4)
        for(int i = optind; i < argc && nbrActifs < 4; i++){
            flux[nbrActifs++] = argv[i];
        }

        if(nbrActifs == 0){
            printf("Au moins un flux d'entree doit etre specifie\n");
            return -1;
        }
    }

    for(int i = 0; i < nbrActifs; i++){
        printf("\tEntree %d : %s\n", i+1, flux[i]);
    }
    printf("Mode d'operation du scheduler modifie avec succes pour compositeur (%d).\n", schedParams.modeOrdonnanceur);
    
    // Changement de mode d'ordonnancement
    appliquerOrdonnancement(&schedParams, "compositeur");

    // Initialiser les zones mémoire partagées en lecteur
    struct memPartage zones[4];
    for(int i = 0; i < nbrActifs; i++){
        if(initMemoirePartageeLecteur(flux[i], &zones[i]) != 0){
            fprintf(stderr, "Erreur d'initialisation de la memoire partagee pour le flux %d\n", i+1);
            return -1;
        }
    }

    // Préparer l'allocateur mémoire (taille max d'une image 427x240x3)
    size_t tailleImage = 427 * 240 * 3;
    if(prepareMemoire(tailleImage, tailleImage) != 0){
        fprintf(stderr, "Erreur d'initialisation de l'allocateur memoire temps reel\n");
        return -1;
    }

    // Cadence cible (fps du premier flux)
    uint32_t fpsCible = zones[0].header->infos.fps;
    uint32_t delaiUsec = (fpsCible > 0) ? (1000000U / fpsCible) : 33333U;

    // Ouvrir (créer) le fichier de stats
    FILE *fstats = fopen("stats.txt", "w");
    if(fstats == NULL){
        // Essayer dans /tmp si le répertoire courant n'est pas accessible
        fstats = fopen("/tmp/stats.txt", "w");
        if(fstats == NULL){
            perror("Erreur ouverture stats.txt");
            return -1;
        }
        printf("Fichier stats cree dans /tmp/stats.txt\n");
    }
    setbuf(fstats, NULL);

    // Variables pour le calcul des stats
    double tempsDebut = get_time();
    double derniereStats = tempsDebut;
    double derniereImage[4] = {tempsDebut, tempsDebut, tempsDebut, tempsDebut};
    int compteurImages[4] = {0, 0, 0, 0};
    double delaiMax[4] = {0.0, 0.0, 0.0, 0.0};

    // Initialisation des structures nécessaires à l'affichage
    long int screensize = 0;
    // Ouverture du framebuffer
    int fbfd = open("/dev/fb0", O_RDWR);
    if (fbfd == -1) {
        perror("Erreur lors de l'ouverture du framebuffer ");
        return -1;
    }

    // Obtention des informations sur l'affichage et le framebuffer
    struct fb_var_screeninfo vinfo;
    struct fb_var_screeninfo orig_vinfo;
    struct fb_fix_screeninfo finfo;
    if (ioctl(fbfd, FBIOGET_VSCREENINFO, &vinfo)) {
        perror("Erreur lors de la requete d'informations sur le framebuffer ");
    }

    // On conserve les précédents paramètres
    memcpy(&orig_vinfo, &vinfo, sizeof(struct fb_var_screeninfo));

    // On choisit la bonne résolution
    vinfo.bits_per_pixel = 24;
    switch (nbrActifs) {
        case 1:
            vinfo.xres = 427;
            vinfo.yres = 240;
            break;
        case 2:
            vinfo.xres = 427;
            vinfo.yres = 480;
            break;
        case 3:
        case 4:
            vinfo.xres = 854;
            vinfo.yres = 480;
            break;
        default:
            printf("Nombre de sources invalide!\n");
            return -1;
            break;
    }

    vinfo.xres_virtual = vinfo.xres;
    vinfo.yres_virtual = vinfo.yres * 2;
    if (ioctl(fbfd, FBIOPUT_VSCREENINFO, &vinfo)) {
        perror("Erreur lors de l'appel a ioctl ");
    }

    // On récupère les "vraies" paramètres du framebuffer
    if (ioctl(fbfd, FBIOGET_FSCREENINFO, &finfo)) {
        perror("Erreur lors de l'appel a ioctl (2) ");
    }

    // On fait un mmap pour avoir directement accès au framebuffer
    screensize = finfo.smem_len;
    unsigned char *fbp = (unsigned char*)mmap(0, screensize, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);

    if (fbp == MAP_FAILED) {
        perror("Erreur lors du mmap de l'affichage ");
        return -1;
    }

    while(1){
        double debutBoucle = get_time();
        double delaiTraitement = 0;
        // Pour chaque flux, vérifier si une image est prête (sans bloquer)
        for(int i = 0; i < nbrActifs; i++){
            evenementProfilage(&profInfos, ETAT_ATTENTE_MUTEXLECTURE);
            int pret = attenteLecteurAsync(&zones[i]);
            if(pret == 0){  // Image prête
                evenementProfilage(&profInfos, ETAT_TRAITEMENT);
                double maintenant = get_time();
                
                // Calculer le délai depuis la dernière image
                double delai = (maintenant - derniereImage[i]) * 1000.0;  // en ms
                if(delai > delaiMax[i]){
                    delaiMax[i] = delai;
                }
                derniereImage[i] = maintenant;
                compteurImages[i]++;
                
                // Afficher l'image
                ecrireImage(i, 
                            nbrActifs, 
                            fbfd, 
                            fbp, 
                            vinfo.xres, 
                            vinfo.yres, 
                            &vinfo, 
                            finfo.line_length,
                            zones[i].data,
                            zones[i].header->infos.hauteur,
                            zones[i].header->infos.largeur,
                            zones[i].header->infos.canaux);
                signalLecteur(&zones[i]);
            }
        }

        // Écrire les stats toutes les 5 secondes
        double maintenant = get_time();
        if(maintenant - derniereStats >= 5.0){
            double tempsEcoule = maintenant - tempsDebut;
            double periode = maintenant - derniereStats;
            
            fprintf(fstats, "[%.1f] ", tempsEcoule);
            for(int i = 0; i < nbrActifs; i++){
                double moyFps = (double)compteurImages[i] / periode;
                fprintf(fstats, "Entree %d: moy=%.1f fps, max=%.1f ms | ", 
                        i+1, moyFps, delaiMax[i]);
                compteurImages[i] = 0;
                delaiMax[i] = 0.0;
            }
            fprintf(fstats, "\n");
            
            derniereStats = maintenant;
        }

        // Respecter la cadence FPS en tenant compte du temps de traitement
        double finBoucle = get_time();
        delaiTraitement = (finBoucle - debutBoucle) * 1000000.0; // en µs

        long tempsRestant = (long)delaiUsec - (long)delaiTraitement;

        evenementProfilage(&profInfos, ETAT_ENPAUSE);

        if(tempsRestant > 0){
            usleep(tempsRestant);
        }
    }


    // cleanup
    fclose(fstats);
    
    // Retirer le mmap
    munmap(fbp, screensize);


    // reset the display mode
    if (ioctl(fbfd, FBIOPUT_VSCREENINFO, &orig_vinfo)) {
        printf("Error re-setting variable information.\n");
    }
    // Fermer le framebuffer
    close(fbfd);

    return 0;

}

