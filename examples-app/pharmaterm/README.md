# PharmaTerm

Plateforme terminal d'achat de médicaments et de conseil pharmaceutique,
écrite en **MATL-ABC**.

Ce projet est avant tout une démonstration de l'architecture en
**sous-projets** : le programme principal n'implémente aucune brique
technique — il assemble quatre sous-projets autonomes, chacun compilable,
testable et réutilisable indépendamment.

```
╔════════════════════════════════════════════════════════════════════╗
║                            PHARMATERM                              ║
║                    Pharmacie du Ponant — Brest                     ║
╚════════════════════════════════════════════════════════════════════╝
  3 article(s) — 12.34 €   ordonnance présentée

┌─ Menu ─────────────────────────────────────────────────────────────┐
│ 1 — Rechercher un médicament ou un symptôme                        │
│ 2 — Parcourir par catégorie                                        │
│ 3 — Voir le panier                                                 │
└────────────────────────────────────────────────────────────────────┘
```

## Architecture

```
pharmaterm/                    ← projet principal, assemble tout
├── main.mabc
├── conf.toml
├── data/
│   └── catalogue.db           ← généré à l'exécution
└── libs/
    ├── search/                ← index de recherche         (aucune dépendance)
    ├── catalog/               ← modèle métier              (depends: search)
    ├── storage/               ← persistance disque         (depends: catalog)
    └── display/               ← termios + rendu ANSI       (aucune dépendance)
```

Ordre de compilation, calculé par tri topologique sur les `depends` :

```
1. search      2. catalog      3. storage      4. display      5. pharmaterm
```

### Comment les sous-projets sont utilisés

Trois conditions, cumulatives — c'est la règle du système de projets
MATL-ABC :

**1. Le module existe** — déclaré comme sous-projet :

```toml
# conf.toml
[subprojects]
paths = ["libs/search", "libs/catalog", "libs/storage", "libs/display"]
```

**2. Le projet a le droit de l'importer** :

```toml
[imports]
libs = ["std", "unistd", "fcntl", "stat", "termios",
        "search", "catalog", "storage", "display"]
```

**3. Le fichier déclare l'utiliser** :

```
/* main.mabc */
import <std>
import <termios>

import <search>
import <catalog>
import <storage>
import <display>
```

Cette troisième ligne est ce qui rend le fichier lisible seul : sans
elle, il faudrait ouvrir `conf.toml` pour savoir d'où viennent
`Pharmacy`, `SearchIndex`, `Terminal` et `CatalogStore`.

La règle vaut **à tous les étages**. `catalog` utilise `search` :

```toml
# libs/catalog/conf.toml
[project]
name    = "catalog"
depends = ["search"]                    # compile search avant moi
exports = ["catalog.mabc"]

[sources]
files = ["../search/search.mabc"]       # je compile contre ses types

[imports]
libs = ["std", "search"]                # j'ai le droit de l'importer
```

```
/* libs/catalog/catalog.mabc */
import <std>
import <search>                          # ce fichier l'utilise
```

Chaque sous-projet expose son API via `exports` ; le compilateur fusionne
automatiquement les modules exportés dans le programme parent, en ordre
topologique.

## Les sous-projets

### `search` — structure de données de recherche

Index inverse avec quatre modes d'interrogation :

| Mode | Comportement |
|------|--------------|
| `EXACT` | la clé est identique au terme |
| `PREFIX` | la clé commence par le terme |
| `SUBSTRING` | le terme apparaît dans la clé |
| `FUZZY` | tolère jusqu'à 2 fautes de frappe (Levenshtein borné) |

Le scoring combine le type de correspondance et un poids par clé : le nom
commercial (50) prime sur la molécule (30), qui prime sur les symptômes
(10).

```
Recherche : paracetmol          ← faute de frappe volontaire
1. Doliprane 500mg   paracetamol   2.15 €   46 en stock
```

### `catalog` — modèle métier

`Medicine`, `Pharmacy`, `CartLine`, avec les règles de délivrance :

```
✔ 2 x Doliprane ajoutés — Opération réussie
✘ Amoxicilline refusée — Ordonnance obligatoire
ℹ Ordonnance présentée
✔ Amoxicilline acceptée — Opération réussie
✘ 99 x Ventoline refusées — Stock insuffisant
```

La `Pharmacy` maintient un `SearchIndex` sur son catalogue : c'est ce
couplage qui justifie `depends = ["search"]`.

Les prix sont en **centimes entiers** — aucun flottant en caisse.

### `display` — termios et rendu

Deux modules :

- **`terminal.mabc`** : wrapper termios (mode brut, lecture de touches
  sans écho, décodage des flèches, restauration garantie de l'état)
- **`display.mabc`** : couleurs ANSI nommées, quatre styles de cadres,
  jauges, alignement conscient de l'UTF-8 et des séquences ANSI

### `storage` — persistance

Format texte à champs séparés, une fiche par ligne :

```
id|nom|molecule|catégorie|statut|prix|stock|posologie|symptômes|conseil
```

Le fichier reste inspectable et corrigeable à la main — ce qui compte
pour un catalogue qu'un titulaire doit pouvoir amender sans outil.

Utilise le wrapper `file` de la bibliothèque standard MATL-ABC
(`OpenMode.WRITE`, `Permission.OWNER_READ_WRITE`) : les wrappers et les
sous-projets se composent naturellement.

## Vérification

```bash
$ matlabc project doctor

Vérification du projet

  ✔ projet cohérent — prêt à compiler
```

`doctor` contrôle fichiers, cycles, exports, `depends` et la cohérence
entre les `import <X>` du code et les déclarations des confs — **sans
lancer LLVM**. Code de sortie `0` si compilable, `1` sinon : utilisable
en pré-commit et en CI.

```bash
$ matlabc project graph

Projet pharmaterm

pharmaterm [exe]
│ utilise : <catalog>, <display>, <search>, <storage>
│ libs C  : fcntl, stat, std, termios, unistd
│ 788 ligne(s), 0 export(s)
├── search [.a]
│     299 ligne(s), 1 export(s)
├── catalog [.a] → search
│     utilise : <search>
│     588 ligne(s), 1 export(s)
├── storage [.a] → catalog
│     utilise : <catalog>, <file>, <search>
│     1002 ligne(s), 1 export(s)
└── display [.a]
      utilise : <terminal>
      477 ligne(s), 2 export(s)

  ─ 5 module(s), 4 bibliothèque(s), 3154 ligne(s)
```

Et en image :

```bash
matlabc project graph --svg
```

## Artefacts

Les bibliothèques compilées vont dans `build/`, jamais à la racine :

```
pharmaterm/
├── build/pharmaterm              ← l'exécutable
└── libs/
    ├── search/build/libsearch.a
    ├── catalog/build/libcatalog.a
    ├── storage/build/libstorage.a
    └── display/build/libdisplay.a
```

Aucun `conf.toml` ne déclare `output` : le chemin est dérivé du
`[project] name` et du `[build] type`.

## Compilation

```bash
matlabc main.mabc --conf conf.toml
```

Le compilateur construit l'arbre en profondeur, en remontant :

```
↳ 4 sous-projet(s), ordre de compilation :
  1. search
  2. catalog (depend de search)
  3. storage (depend de catalog)
  4. display
↳ 5 module(s) exporté(s) par les sous-projets
✔ Exécutable produit : ./pharmaterm
```

Chaque sous-projet peut aussi se construire seul :

```bash
cd libs/search && matlabc search.mabc --conf conf.toml
```

## Exécution

```bash
./pharmaterm
```

- **Terminal interactif** → mode brut, navigation au clavier
- **Sortie redirigée** → démonstration scriptée exerçant toutes les
  briques (le programme détecte l'absence de terminal via `tcgetattr`)

## Conventions

- **Identifiants en anglais, commentaires en français**
- Structs avec méthodes plutôt que fonctions libres
- `enum` typés partout — aucune constante magique
- `alloc<T>(n)` plutôt que `cast<T*>(malloc(...))`
- `str` natif, `.data()` confiné aux `printf("%s")`
- `match` plutôt que les cascades de `else if`
- Prix en entiers, jamais en flottants

## Pièges MATL-ABC rencontrés

Consignés pour la postérité — ils ont tous coûté un cycle de debug :

| Piège | Symptôme | Remède |
|-------|----------|--------|
| `header`, `word`, `type`, `block`, `view` sont des **mots-clés** | erreur syntaxique cryptique | renommer la variable |
| `truncate` existe dans **unistd** | IR invalide, `store` de types incompatibles | renommer la fonction |
| `data()` retourne un `char*` **signé** | octet UTF-8 `0xE2` vaut `-30`, largeurs faussées | masquer avec `& 255` |
| `\|` dans une f-string | erreur syntaxique | concaténer hors f-string |
| listes TOML multi-lignes | `Ligne invalide (pas de '=')` | tout sur une ligne |

## Licence

Exemple pédagogique fourni avec le compilateur MATL-ABC.
