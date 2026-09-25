# Audacity Quick Edit pour Zetta

Cette version d'Audacity ajoute un mode **quick edit** : Audacity ouvre un fichier audio donné par une autre application, comme RCS Zetta, et **Ctrl+S réécrit ce fichier**, au même endroit, sous le même nom et dans le même format, puis ferme Audacity. Zetta récupère alors le fichier modifié.

Sans `--quick-edit`, Audacity fonctionne normalement.

---

## Configuration dans Zetta

Dans la configuration de l'éditeur externe de Zetta :

| Champ Zetta | Valeur |
|---|---|
| Programme | Le chemin d'Audacity, par exemple `C:\Program Files\Audacity\Audacity.exe` |
| Arguments | `--quick-edit %f` (minimum) |

Ligne complète avec toutes les options :
```
--quick-edit --live-dir "\\SERVEUR\LiveRec" --record-format mp2-256 --backup-dir "D:\Sauvegardes Audacity" %f
```

À respecter :
- **`%f` à la fin**, après toutes les options.
- **Ne pas écrire `Audacity.exe` dans les arguments**. Il va seulement dans le champ Programme.
- Les chemins qui contiennent des espaces se mettent entre guillemets : `"D:\Sauvegardes Audacity"`.
- Pour `%f` : Audacity accepte `%f` et `"%f"`, et recolle un chemin qui arriverait coupé. En cas de problème, essayez l'autre forme.

---

## Les options

Toutes les options sont **facultatives**, sauf `--quick-edit`.

| Option | Variable d'environnement | Rôle | Sans l'option |
|---|---|---|---|
| `--quick-edit` | | Active le mode quick edit. **Obligatoire.** | Audacity normal |
| `--live-dir "<dossier>"` | `AU_LIVE_RECORD_DIR` | Pendant un enregistrement, copie le son en direct dans un WAV de ce dossier réseau (voir [Enregistrement live](#enregistrement-live)) | Pas de copie live |
| `--record-format <format>` | `AU_RECORD_FORMAT` | Format des **nouveaux** fichiers (enregistrements) | WAV, ou MP2 si le nom finit par `.mpg` |
| `--backup-dir "<dossier>"` | `AU_QUICK_EDIT_BACKUP_DIR` | Dossier des sauvegardes | `%TEMP%\Audacity Quick Edit Backups` |

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
| Un fichier déjà ouvert dans un autre Audacity | Affiche « déjà ouvert » et ne relance pas d'enregistrement. Le dernier Ctrl+S l'emporte. |

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

Si un Audacity est **déjà ouvert** sur le poste (lancé à la main, éventuellement réduit) :
- chaque édition lancée depuis Zetta s'ouvre **tout de suite** dans une nouvelle fenêtre de cet Audacity ;
- après Ctrl+S, seule cette fenêtre se ferme, et l'Audacity de fond reste prêt pour la suivante ;
- Zetta récupère le fichier normalement.

Sans Audacity ouvert, chaque édition démarre son propre Audacity, ce qui prend quelques secondes.

**Conseil :** pour l'avoir toujours prêt, mettez un raccourci vers Audacity dans le dossier de démarrage de Windows (`Win+R` → `shell:startup`).

On peut avoir en même temps une fenêtre qui **enregistre** et une autre qui **fait un montage**.

---

## Enregistrement live

Avec `--live-dir "\\SERVEUR\LiveRec"`, un enregistrement lancé depuis Zetta est **copié en direct** dans le dossier réseau :

| Fichier | Contenu |
|---|---|
| `<nom>_<date>.wav` | Le son, en WAV 16 bits, qui grossit environ toutes les secondes. Il peut être ouvert à tout moment. |
| `<nom>_<date>.json` | L'état : `recording` (en cours) ou `done` (terminé), et le fichier Zetta visé |

L'enregistrement dans Audacity et l'écriture du fichier Zetta ne changent pas. Si le réseau est lent ou coupé, l'enregistrement n'est pas bloqué.

### Suivre un enregistrement live depuis un autre poste

1. Sur le poste de montage, ouvrez le `.wav` en cours depuis le dossier réseau, par **glisser-déposer** ou **Fichier > Ouvrir**.
2. **Toutes les 2 secondes, la suite s'ajoute** au clip. Une notification indique la fin de l'enregistrement.
3. **Montez sur une copie du clip**, ou sur une autre piste : le clip live s'allonge par la fin.
4. Ctrl+S enregistre le montage dans **`<nom>_montage.wav`**, à côté du fichier live, **jamais par-dessus**.

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

Le journal d'Audacity se trouve dans `%LOCALAPPDATA%\Audacity\`, probablement sous `Audacity4Development\logs`. Il note le fichier reçu, les étapes du démarrage et les enregistrements live.

---

## Mettre à jour cette version

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
