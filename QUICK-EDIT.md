# Audacity Quick Edit pour Zetta

Cette version d'Audacity ajoute un mode **quick edit** : Audacity ouvre un fichier audio donné par une autre application, comme RCS Zetta, et **Ctrl+S réécrit ce fichier**, au même endroit, sous le même nom et dans le même format, puis ferme Audacity. Zetta récupère alors le fichier modifié.

Sans `--quick-edit`, Audacity fonctionne normalement.

---

## Configuration dans Zetta

Dans la configuration de l'éditeur externe de Zetta :

| Champ Zetta | Valeur |
|---|---|
| Programme | Le chemin d'Audacity, par exemple `C:\Program Files\Audacity\Audacity.exe` |
| Arguments (même valeur sur les postes Enregistrer et Monter) | `--live-record --live-dir "\\SERVEUR\LiveRec" "%f"` |

Ajoutez les mêmes options facultatives aux deux actions si nécessaire :
```
--quick-edit --live-dir "\\SERVEUR\LiveRec" --record-format mp2-256 --backup-dir "D:\Sauvegardes Audacity" %f
```
`--live-record` prépare une session sans lancer la capture : clique sur **Enregistrer** dans Audacity
quand tu es prêt. Si une session correspond déjà au chemin Zetta transmis, Audacity ouvre son WAV live
pour montage, sans démarrer une deuxième capture. Si aucune session n'existe et que Zetta transmet
un fichier déjà enregistré, Audacity l'ouvre directement en quick-edit, comme avant.

À respecter :
- **`%f` à la fin**, après toutes les options.
- **Ne pas écrire `Audacity.exe` dans les arguments**. Il va seulement dans le champ Programme.
- Les chemins qui contiennent des espaces se mettent entre guillemets : `"D:\Sauvegardes Audacity"`.
- Pour `%f` : Audacity accepte `%f` et `"%f"`, et recolle un chemin qui arriverait coupé. En cas de problème, essayez l'autre forme.

---

## Les options

Toutes les options sont **facultatives**, sauf `--quick-edit` ou `--live-record`.

| Option | Variable d'environnement | Rôle | Sans l'option |
|---|---|---|---|
| `--quick-edit` | | Ouvre un fichier existant pour montage. À utiliser seul ou avec `--live-dir`. | Audacity normal |
| `--live-record` | | Démarre une capture, ou rejoint la session du même fichier Zetta si elle est déjà en cours. Implique `--quick-edit` et exige `--live-dir`. | Non applicable |
| `--live-dir "<dossier>"` | `AU_LIVE_RECORD_DIR` | Pendant un enregistrement, copie le son en direct dans un WAV de ce dossier réseau (voir [Enregistrement live](#enregistrement-live)) | Pas de copie live |
| `--record-format <format>` | `AU_RECORD_FORMAT` | Format des **nouveaux** fichiers (enregistrements) | WAV, ou MP2 si le nom finit par `.mpg` |
| `--backup-dir "<dossier>"` | `AU_QUICK_EDIT_BACKUP_DIR` | Dossier des sauvegardes | `%TEMP%\Audacity Quick Edit Backups` |
| `--new-instance` | `AU_ALLOW_MULTIPLE_PROCESSES` | Ouvre un processus séparé avec son propre moteur audio | Réutilise Audacity, sauf pour les sessions live qui sont isolées automatiquement |

### Valeurs de `--record-format`

| Valeur | Format |
|---|---|
| `wav16` | WAV 16 bits |
| `wav24` | WAV 24 bits |
| `wav32f` | WAV 32 bits flottant |
| `mp2-<kbps>` | MP2 (MPEG-1 Layer II), par exemple `mp2-256`, `mp2-192` |
| `mp3-<kbps>` | MP3 à débit constant, par exemple `mp3-320`, `mp3-192` |

`--record-format` ne concerne **que les nouveaux fichiers**. Un fichier existant garde toujours son format d'origine.

### Option ou variable d'environnement ?

Chaque option peut être remplacée par une variable d'environnement Windows. C'est utile pour activer une option **sur certains postes seulement**, sans changer la configuration de Zetta.

Pour la définir : menu Démarrer → « variables d'environnement » → **Modifier les variables d'environnement système** → **Variables d'environnement…** → **Nouvelle…**, puis **redémarrer Zetta**.

Si l'option et la variable sont toutes les deux présentes, l'option de la ligne de commande est prioritaire.

---

## Ce qui se passe selon le fichier

| Zetta envoie… | Audacity… |
|---|---|
| Un fichier audio existant | L'ouvre pour le modifier. Ctrl+S le réécrit, puis ferme Audacity. |
| Un fichier **vide ou inexistant** (enregistrement) | Ouvre un projet vide et **lance l'enregistrement tout de suite**. Ctrl+S, même pendant l'enregistrement, arrête, écrit le fichier et ferme. |
| Un fichier existant en mode `--live-record`, sans session correspondante | L'ouvre directement en quick-edit, comme avant. |
| Un fichier vide ou inexistant en mode `--live-record`, sans session correspondante | Prépare une nouvelle session sans démarrer la capture ; le bouton **Enregistrer** démarre la capture. |
| Un fichier en mode `--live-record`, avec session correspondante | Ouvre le WAV partagé de cette session dans un éditeur de montage. N'en démarre pas une deuxième. |
| Un fichier déjà ouvert dans un autre Audacity | Affiche « déjà ouvert » et ne relance pas d'enregistrement. Le dernier Ctrl+S l'emporte. |
| Le même fichier Zetta qu'une session live active, avec le même `--live-dir` | Retrouve l'émission et ouvre son audio live dans un éditeur indépendant, même si le fichier Zetta est encore vide. Aucun deuxième enregistrement ne démarre. |

**Exception avec `--live-dir` :** le nouvel enregistrement appartient à une session de montage live.
L'arrêt, ou Ctrl+S dans la fenêtre d'enregistrement, ne renvoie **pas** le brut à Zetta.
Cette fenêtre attend le montage final validé et reste ouverte jusqu'à sa publication.

### Formats reconnus

Le format est reconnu **d'après le contenu du fichier**, pas son nom. Zetta utilise des noms comme `titre.-12283` ou des MP3 nommés `.mpg`.

- WAV, AIFF, FLAC, Ogg Vorbis, Opus
- MP2 et MP3, même avec l'extension `.mpg` ou sans extension. Le **débit d'origine est conservé** (MP3 à débit constant).
- WAV et FLAC gardent leur **nombre de bits d'origine**.

### Sélection

Si une partie du son est sélectionnée au moment du Ctrl+S, Audacity demande :
- **Selection only** : le fichier est remplacé par la sélection seule ;
- **Whole file** : tout le son est enregistré ;
- **Cancel** : rien n'est écrit.

### Accents dans les noms

Les noms et dossiers accentués (`Forêt`, `Écho`…) sont gérés, quelle que soit la façon dont Zetta les transmet.

---

## Démarrage instantané

Pour une édition normale, **sans session live ni `--new-instance`**, si un Audacity est déjà ouvert sur le poste :
- chaque édition lancée depuis Zetta s'ouvre **tout de suite** dans une nouvelle fenêtre de cet Audacity ;
- après Ctrl+S, seule cette fenêtre se ferme, et l'Audacity de fond reste prêt pour la suivante ;
- Zetta récupère le fichier normalement.

Sans Audacity ouvert, chaque édition démarre son propre Audacity, ce qui prend quelques secondes.

**Conseil :** pour l'avoir toujours prêt, mettez un raccourci vers Audacity dans le dossier de démarrage de Windows (`Win+R` → `shell:startup`).

Pour le montage live, les processus sont **séparés automatiquement** : les fenêtres d'un même processus
partagent un moteur audio, ce qui ne permet pas de garantir une écoute indépendante pendant l'enregistrement.
L'isolation prend le temps d'un démarrage Audacity, mais évite que le montage utilise le moteur qui enregistre.

---

## Enregistrement live

Avec `--live-dir "\\SERVEUR\LiveRec"`, un enregistrement lancé depuis Zetta est **copié en direct** dans le dossier réseau :

| Fichier | Contenu |
|---|---|
| `<nom>_<date>_<id>.wav` | Le brut, en WAV 16 bits, qui grossit environ toutes les secondes. |
| `<nom>_<date>_<id>.json` | L'état : `recording`, `done` ou `error`, et le fichier Zetta visé. |
| `<nom>_<date>_<id>.final.wav` | Le rendu final figé, en WAV flottant, déposé par l'éditeur qui valide. |
| `<nom>_<date>_<id>.montage.json` | Validation du montage et empreinte SHA-256 du rendu. |
| `<nom>_<date>_<id>.result.json` | Confirmation que le poste d'enregistrement a écrit le montage dans le fichier Zetta. |
| `zetta-target-<empreinte>.json` et son `.lock` | Association du chemin Zetta complet à l'émission live ; réservation détenue par le poste qui enregistre jusqu'à la fermeture ou l'annulation. |

L'identifiant est unique, même si plusieurs enregistrements démarrent dans la même seconde.
Le fichier live brut n'est jamais remplacé par un montage. Ne modifiez pas les fichiers de coordination
ni le rendu `.final.wav` après validation. Les JSON et le rendu final sont publiés par écriture atomique.
Le WAV live classique est limité à moins de 4 Go ; un dépassement est signalé comme une erreur.

### Plusieurs émissions et ouverture depuis un autre Zetta

Configurez **tous les Zetta concernés** avec `--live-record --live-dir "<même dossier partagé>" "%f"`.
Le Zetta utilisé pour monter doit transmettre **le même chemin audio complet** que celui utilisé
pour enregistrer cette émission. Audacity utilise ce chemin, et non le seul nom du fichier,
pour retrouver la session :

| Action depuis Zetta | Résultat |
|---|---|
| Ouvrir l'émission A avec `--live-record`, sans session active pour son fichier | Réserve A et ouvre un projet prêt ; la capture démarre uniquement quand l'opérateur clique sur **Enregistrer**. |
| Enregistrer l'émission B dans un autre fichier | Démarre une autre session indépendante ; A continue. |
| Ouvrir A pour montage dans un autre Zetta avec les mêmes arguments | Ouvre le live de A, même si le fichier audio Zetta n'a pas encore été écrit. |
| Ouvrir B pour montage | Ouvre uniquement le live de B. |
| Ouvrir plusieurs montages de A et de B | Chaque éditeur possède son brouillon distinct et reste rattaché à son émission. |
| Valider le montage final de A | Attend seulement l'arrêt de A ; n'arrête pas B et ne publie aucun montage de B. |

Il n'est plus nécessaire de chercher manuellement le WAV partagé quand l'ouverture part de Zetta.
Si l'émission vient de démarrer, l'éditeur attend jusqu'à 30 secondes les premiers échantillons.
Un verrou partagé empêche deux postes d'enregistrer simultanément dans **le même fichier Zetta**.
Si ce fichier est déjà associé à une émission live, l'ouverture devient un montage de cette émission.

Sans l'option expérimentale `--shared-montage`, les éditeurs suivent **le même enregistrement**, mais ne modifient pas simultanément la même base
de projet `.aup3` : les montages sont indépendants, pas une timeline collaborative.
Comme convenu, **un seul montage final par émission** est transmis à Zetta.

Pour identifier une émission entre postes, utilisez le même chemin réseau, idéalement UNC
(`\\SERVEUR\Zetta\Audio\...`). La casse et les séparateurs Windows sont normalisés.
Un chemin UNC et un lecteur réseau mappé (`Z:\...`) ne sont pas automatiquement considérés comme identiques ;
des copies temporaires différentes ne peuvent pas être associées sans identifiant Zetta supplémentaire.

### Montage partagé entre postes — option expérimentale

L'option `--shared-montage` (ou `AU_SHARED_MONTAGE=1`) ajoute une timeline commune
au fonctionnement live. **Cette implémentation n'a pas encore été compilée ni validée
sur deux postes Zetta dans cet environnement.** Gardez le mode actuel pour la production
tant que les essais ci-dessous ne sont pas terminés.

Configurez les postes participant au même montage avec :

```text
"C:\chemin\Audacity.exe" --live-record --shared-montage --live-dir "\\SERVEUR\AudacityLive" "%f"
```

**La même configuration s'applique à tous les postes.** Aucun ordinateur n'a un rôle
permanent d'enregistreur ou de monteur. Le rôle est déterminé pour chaque fichier Zetta :
ouvrir un nouveau fichier sans session active permet de lancer le REC ; ouvrir un fichier
dont la session existe rejoint son montage. Un poste qui monte une émission peut donc
enregistrer la suivante, et celui qui a enregistré peut ensuite rejoindre un montage.
Les ouvertures supplémentaires utilisent des processus Audacity indépendants.

Par exemple, le poste A peut enregistrer l'émission 1 pendant que B la monte, puis B
enregistrer l'émission 2 pendant que A la monte. A peut aussi monter l'émission 1 pendant
son propre REC grâce à la fenêtre de montage ouverte automatiquement. Pour chaque session,
la fenêtre qui détient la capture conserve le fichier Zetta d'origine et réalise l'export
final ; changer de poste de montage ne change pas le destinataire de cet export.

#### Affichage des postes présents dans Audacity

Pour une session live, la barre d'état comporte un indicateur cliquable, par exemple
**REC : Studio A · Montage : 2 postes**. Le point rouge indique une présence en REC ;
le bleu correspond à la session de montage et l'orange à un partage indisponible.
Un clic affiche les noms des postes et le rôle de chaque fenêtre : prêt pour REC,
enregistrement, montage, montage local avec synchronisation suspendue, montage envoyé,
montage figé ou erreur. La fenêtre actuelle est identifiée dans la liste.

Les noms sont ceux des ordinateurs. Pour afficher un nom plus parlant, définissez
`AU_STATION_NAME` sur chaque poste avant de lancer Audacity, par exemple `Studio A`
ou `Montage 2`. Un poste peut apparaître à la fois en REC et en montage si deux de
ses fenêtres participent à la session. Le compteur de montage compte les postes
distincts, pas le nombre de fenêtres. L'indicateur est masqué pour un projet ordinaire.

La présence est publiée toutes les deux secondes dans le dossier `<session>.presence`.
Les lectures et les écritures de présence sont réalisées en arrière-plan pour qu'un
partage lent ne bloque pas l'interface pendant le REC ou le montage.
Une fermeture normale retire la fenêtre de la liste au prochain rafraîchissement.
Un signal inchangé expire après environ quinze secondes d'observation, notamment après une
panne. Le délai est mesuré avec l'horloge monotone du lecteur : les horloges des postes
n'ont pas besoin d'être identiques. À l'ouverture de la liste, une ancienne présence
peut apparaître pendant ce délai initial. Cet affichage est informatif ; les verrous
de session continuent de décider qui possède le REC et l'export Zetta.

Les tests `LiveSessionPresenceTests` couvrent le changement de rôle, l'expiration et
la reprise des présences, plusieurs fenêtres sur un même poste, la fermeture, la
séparation des émissions et les fichiers de présence invalides. Comme le montage
partagé, cet affichage reste à compiler et à vérifier dans l'application sur deux postes.

Le chemin audio Zetta transmis par `%f` doit être identique entre les postes,
comme dans le mode live existant. Cette option ne crée pas un identifiant commun
pour des fichiers temporaires Zetta portant des chemins différents.

1. Créez le nouveau fichier dans Zetta et ouvrez-le. Audacity réserve la session ;
   dans ce mode, cliquez sur **REC** pour commencer.
2. Dès que les premiers échantillons sont disponibles, une fenêtre de montage
   indépendante s'ouvre automatiquement sur le poste qui enregistre. Elle possède
   son propre moteur audio ; la capture continue dans la fenêtre d'enregistrement.
3. Ouvrez le même fichier depuis un autre Zetta avec les mêmes options. Cet Audacity
   rejoint le montage partagé. L'audio nouveau est ajouté automatiquement après
   les passages déjà montés. Couper, déplacer ou supprimer le clip de fin ne remet
   pas les anciennes coupes à zéro : l'arrivée suivante continue dans un nouveau clip.
4. Les changements de montage sont publiés et recherchés à intervalles d'une seconde.
   L'arrivée de l'audio reste vérifiée toutes les deux secondes. Les temps réels
   dépendent de la durée du projet et du débit du partage. Une réception de montage
   attend la fin d'un déplacement en cours ou de la lecture audio.
5. Si deux postes modifient une même version avant de recevoir le changement de l'autre,
   le premier publié est conservé. Le second montage est sauvegardé dans un fichier
   `.conflict-<id>.aup3`, puis Audacity propose de charger la version commune. Annuler
   conserve le travail local et suspend sa synchronisation ; rouvrir depuis Zetta
   permet de rejoindre à nouveau la version partagée. **Il n'y a pas de fusion
   automatique des modifications concurrentes.**
6. Le montage reste modifiable pendant le REC et après son arrêt tant qu'une fenêtre
   de montage participe à la session. Fermer une fenêtre de montage, ou choisir
   **Montage terminé** dans cette fenêtre, synchronise ses dernières modifications
   puis quitte uniquement cette fenêtre. Un conflit non résolu ou une erreur du
   partage empêche le départ ; les autres monteurs et le REC continuent.
7. Le poste d'enregistrement attend **l'arrêt du REC et le départ de toutes les fenêtres
   de montage**. Il verrouille alors la version commune, conserve la sauvegarde brute
   et exporte dans le fichier Zetta avec le circuit actuel. Les formats, sauvegardes
   et confirmations Zetta restent ceux du mode existant. Gardez la fenêtre de capture
   ouverte jusqu'à cet export ; elle se ferme après confirmation de publication.
8. Si tous les monteurs quittent pendant le REC, la capture continue. Un monteur peut
   rejoindre à nouveau tant que la session n'est pas clôturée. À la fin du REC, le
   poste d'enregistrement complète la dernière version avec les échantillons non
   encore reçus, sans rétablir les coupes précédentes, puis publie le résultat.

Le brut n'apparaît pas comme une piste à modifier dans les éditeurs partagés :
il est conservé à part afin que les coupes et Annuler/Rétablir ne touchent pas le REC.
Ctrl+S conserve son rôle de sauvegarde d'un brouillon audio ; il ne remplace pas la
clôture collective et n'écrit pas directement dans le fichier Zetta depuis un éditeur.

Les postes échangent des **copies immuables** de projets natifs :
`.shared-montage.json` désigne la version courante, avec son empreinte SHA-256,
et `.montage-<version>.aup3` contient la copie correspondante. Chaque poste charge
une copie locale et transfère les blocs audio dans sa propre base de projet.
Le fichier `.montage.json` reste exclusivement le rendu final utilisé par Zetta.
Le registre `.editors.json` garde les participations jusqu'à leur départ explicite,
puis scelle la révision sous le même verrou que les arrivées et les modifications.
L'expiration de l'indicateur de présence ne libère pas cette participation : une panne
ou une coupure réseau ne doit pas publier un travail inachevé. Une fenêtre qui plante
avant son départ laisse donc la clôture en attente ; la récupération de cette
participation doit être effectuée avant l'export (pas de retrait automatique dans
ce prototype). La récupération ergonomique après panne reste à prévoir avant production.
Les anciennes copies sont conservées pour les lecteurs en cours et la récupération ;
leur taille cumulée peut être importante. Ce prototype copie le projet complet lors
des changements : mesurer la fluidité et l'espace disque sur des émissions longues
avant tout déploiement.

Essais à effectuer avec une compilation complète : ouverture depuis deux postes,
montage pendant le REC, arrivée de l'audio après coupe du clip de fin, Annuler/Rétablir
pendant l'arrivée du direct, lecture pendant une modification distante, conflit entre
deux éditeurs, perte du partage, puis départs avant/après l'arrêt du REC et vérification
du fichier final dans Zetta. Vérifier aussi le départ de tous les monteurs pendant
le REC, la conservation de l'audio capturé ensuite, une réouverture avant clôture,
un conflit au départ et une arrivée concurrente à la clôture. Les tests `LiveRecordSessionTests` couvrent aussi les
révisions périmées, l'intégrité des copies et la conservation du circuit de publication.
Répéter les essais en inversant les rôles de A et B pour l'émission suivante, et vérifier
que A peut monter pendant que B enregistre sans modifier la configuration des postes.

Sans `--shared-montage` et sans `AU_SHARED_MONTAGE=1`, le mode précédent reste actif.

### Montage en temps réel, sur le même poste ou sur un autre

1. Configurez l'action Zetta utilisée pour ouvrir l'audio sur tous les postes avec
   `--live-record --live-dir "<dossier partagé>" "%f"`.
   Le premier poste démarre la capture ; les autres ouvrent le même direct pour montage.
2. Sur un autre Zetta, demandez le montage de **la même émission** : Audacity retrouve le live automatiquement.
   Sur le poste d'enregistrement, **Fichier > Ouvrir un montage live** permet aussi d'ouvrir plusieurs éditeurs.
   En dehors de Zetta, vous pouvez ouvrir le **WAV brut** de la session par **Fichier > Ouvrir**.
   Depuis une fenêtre qui enregistre, ouvrez-le plutôt que de l'importer comme une piste supplémentaire.
   Un éditeur indépendant est lancé si nécessaire. Un lancement explicite est également possible :
   ```
   Audacity.exe --new-instance --quick-edit "\\SERVEUR\LiveRec\<nom>_<date>_<id>.wav"
   ```
3. Audacity crée une **copie de travail** du son déjà disponible et garde la piste source **muette**.
   Toutes les deux secondes, la suite est ajoutée à cette piste source, identifiée indépendamment de
   l'ordre des pistes. Ne coupez pas et ne supprimez pas la source live : copiez les nouveaux passages
   vers les pistes de montage. La copie de travail n'est pas rallongée automatiquement, pour préserver vos coupes.
4. Ctrl+S sauvegarde un **brouillon** `<nom>_<date>_<id>_montage_<éditeur>.wav` sans fermer l'éditeur
   et **sans l'envoyer à Zetta**. Chaque éditeur possède son propre identifiant pour éviter les écrasements.
5. Dans **la fenêtre choisie pour le montage final**, faites **Fichier > Montage terminé**
   (`Montage finished` en anglais). Confirmez pour figer **tout le montage audible**, sans la source live.
   La sélection temporelle ne limite pas ce rendu ; pour ne garder qu'un passage, montez-le sur les pistes de travail.
6. Arrêtez l'enregistrement sur le poste d'enregistrement. L'ordre des étapes 5 et 6 est libre :
   la publication attend **les deux conditions**.
7. Le poste d'enregistrement sauvegarde le brut en projet `.aup3`, vérifie le rendu final,
   le convertit dans le format prévu pour Zetta et réécrit **le fichier fourni par Zetta**.
   Il confirme la publication et ferme sa fenêtre. Zetta peut alors récupérer le fichier.
   L'éditeur se ferme aussi après confirmation, sauf si des modifications ont été faites après la validation.

**Validation anticipée :** les secondes enregistrées après « Montage terminé » et les modifications
ultérieures de l'éditeur **ne font pas partie du rendu final**. Le rendu validé est un instantané.
Un verrou partagé permet à **un seul éditeur** de valider cette session ; une deuxième validation est refusée.

Le poste de montage n'a pas besoin d'accéder au chemin temporaire local de Zetta.
**Seul le poste qui enregistre écrit ce fichier**, ce qui fonctionne aussi entre ordinateurs.
Il doit rester ouvert et connecté jusqu'à la publication ; arrêter l'enregistrement ne suffit pas à le fermer.

### Conditions et récupération

- Tous les postes doivent utiliser cette version et accéder au **même dossier partagé**, avec des droits
  de lecture et d'écriture et une prise en charge des verrous/créations exclusives et des renommages atomiques.
- Sur un même ordinateur, le pilote audio doit permettre l'enregistrement et la lecture par des processus
  différents. Un périphérique ou pilote en mode exclusif peut les empêcher ; utilisez un mode partagé
  compatible ou un autre périphérique de sortie pour le montage.
  Plusieurs émissions demandent aussi des entrées/sources audio adaptées : Audacity n'attribue pas
  automatiquement un périphérique ou un canal différent à chaque émission.
- Une erreur d'écriture du live, de validation du rendu ou d'export empêche la publication et affiche une erreur.
  Après correction d'un problème temporaire, Ctrl+S dans la fenêtre concernée relance la tentative.
  L'arrêt attend la fin des écritures en cours sur le partage.
- Pour abandonner, utilisez **Fichier > Annuler le montage live** dans la fenêtre d'enregistrement.
  L'enregistrement s'arrête et la session est marquée en erreur, sans publier le montage.
  Le brut reste dans le projet et peut être sauvegardé manuellement.
- Après une panne ou une fermeture forcée du poste d'enregistrement, il n'y a pas de reprise automatique
  de la publication. Une association partagée sans propriétaire bloque l'ouverture au lieu de démarrer
  un deuxième enregistrement. Conservez les WAV et les sauvegardes `.aup3` pour récupération.
  Après récupération et vérification qu'aucun poste n'utilise cette session, l'administrateur peut retirer
  uniquement l'association `zetta-target-<empreinte>.json` de cette émission pour autoriser un nouveau départ.

### Vérification après compilation

Les tests `LiveRecordSessionTests` du module `au_project_tests` couvrent les deux ordres de fin,
le rendu figé, les validations concurrentes, les fichiers incomplets/modifiés et l'accusé de publication.
Ils couvrent aussi la recherche par chemin Zetta, les émissions de même nom dans des dossiers différents,
les ouvertures multiples, la réservation d'enregistrement et les associations orphelines.
Ils doivent être complétés par un essai réel avec Zetta :

1. Écouter et monter dans un éditeur pendant un enregistrement ; vérifier que celui-ci continue.
2. Valider le montage avant l'arrêt, puis tester l'ordre inverse. Le fichier Zetta ne doit être écrit
   qu'après les deux étapes et doit contenir le montage, pas le brut. Sa durée doit être celle des
   pistes audibles du montage, sans silence final ajouté à cause de la piste source muette.
3. Ouvrir deux éditeurs de la même session : seule la première validation doit être acceptée.
4. Tester entre deux postes et vérifier que seul le poste d'enregistrement accède au fichier Zetta.
5. Enregistrer un signal jusqu'à l'arrêt et vérifier que la fin du WAV live n'est pas tronquée.
6. Simuler une indisponibilité du partage ou un fichier Zetta verrouillé : aucune réussite ne doit
   être annoncée et les fichiers de récupération doivent être conservés.
7. Depuis deux Zetta, lancer A et B puis ouvrir plusieurs montages par les **chemins Zetta d'origine**,
   sans sélectionner les WAV live manuellement. Vérifier qu'aucun deuxième enregistrement de A ne démarre
   et que la publication de A ne modifie ni l'enregistrement, ni les brouillons, ni le fichier Zetta de B.

---

## Glisser-déposer

- **Un son glissé dans une édition lancée par Zetta** s'ajoute au projet. Ctrl+S écrit toujours dans **le fichier de Zetta**, sous son nom d'origine. Le fichier glissé n'est pas modifié.
- **Un fichier glissé depuis l'Explorateur dans une fenêtre Audacity vide** s'ouvre en quick edit : Ctrl+S réécrit ce fichier.
- Un format qu'Audacity ne sait pas réécrire, comme `.m4a` ou une vraie vidéo `.mpg`, s'ouvre en projet normal.

---

## Sauvegardes

À chaque Ctrl+S, **avant** d'écraser le fichier, Audacity range dans un sous-dossier daté du dossier des sauvegardes :
- le **fichier d'origine**, tel qu'il était avant la modification ;
- le **projet Audacity** (`.aup3`), qu'on peut rouvrir pour reprendre le montage.

Les sauvegardes de plus de **5 jours** sont supprimées automatiquement. Si l'original ne peut pas être sauvegardé, **le fichier n'est pas écrasé**.

Emplacement : `%TEMP%\Audacity Quick Edit Backups`, ou le dossier choisi avec `--backup-dir`.

---

## Dépannage

| Message ou symptôme | Cause | Solution |
|---|---|---|
| « Unknown options: ., m, p, g » | Chemin `%f` coupé aux espaces (guillemets) | Essayer `%f` ou `"%f"` dans Zetta |
| Audacity veut écrire dans `…\Zetta\Audacity.exe` | `Audacity.exe` écrit dans les arguments Zetta | Le mettre seulement dans le champ Programme |
| Audacity s'ouvre vide avec « Recording into… » alors qu'on voulait éditer | Le fichier reçu n'existe pas (mauvais chemin) | Vérifier le chemin affiché dans la notification |
| « Could not determine an export format » | Format de fichier non reconnu | Envoyer le fichier pour analyse |
| « Could not write … » | Fichier verrouillé ou en lecture seule | Le son exporté est conservé dans le dossier indiqué par le message |
| Zetta ne prend pas la modification | Audacity n'est pas fermé | Faire Ctrl+S (qui ferme), ou fermer la fenêtre |
| Session live en attente | Enregistrement non arrêté ou montage non validé | Arrêter l'enregistrement et faire « Montage terminé » dans l'éditeur final |
| Impossible de fermer la fenêtre d'enregistrement live | Publication non terminée | Garder la fenêtre ouverte, corriger l'erreur et réessayer avec Ctrl+S, ou annuler explicitement le montage live |

Le journal d'Audacity se trouve dans `%LOCALAPPDATA%\Audacity\`, probablement sous `Audacity4Development\logs`. Il note le fichier reçu, les étapes du démarrage et les enregistrements live.

---

## Mettre à jour cette version

### Compilation Windows avec un MSVC récent

Le projet reste en C++17 et Muse utilise encore les coroutines expérimentales.
La configuration MSVC définit `_SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS`
pour les compilations C++ afin d'éviter l'erreur `STL1011` des outils récents,
y compris sur ARM64. Il s'agit d'une compatibilité temporaire avec l'API existante,
pas d'une migration vers les coroutines C++20. Si Microsoft supprime cette API,
une mise à jour de Muse sera nécessaire.

Le code est sur la branche `quick-edit` de https://github.com/mickael75/audacity.

1. Récupérer les dernières modifications (dans le dossier `au-quickedit`) :
   ```sh
   git pull
   ```
2. Compiler : sur GitHub, **Actions → [AU4] Build: Windows → Run workflow**, branche `quick-edit`, `devel_build`. Compter environ 35 minutes.
3. Télécharger : dans le run terminé, **Summary → Artifacts**, l'artifact **x86_64** (et non celui qui commence par `selftest-`).
4. Installer : décompresser, puis lancer le `.msi`.

Pour pousser vos propres modifications :
```sh
git add -A
git commit -m "Description du changement"
git push
```
