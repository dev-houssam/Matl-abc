# matyacc — générateur de parser pour MATL-ABC

Équivalent de `bison`/`menhir` : compile une grammaire hors-contexte
(fichier `.may`) en un **parseur SLR(1)** (automate à pile,
shift-reduce), écrit **en MATL-ABC lui-même** — projet frère de
`matlex`, même philosophie : un vrai problème d'ingénierie plutôt que
des cas jouets, testé jusqu'à obtenir un binaire autonome qui calcule
correctement.

## Sommaire des rounds

| Round | Fichier | Contenu | Validation |
|---|---|---|---|
| 1 | `src/lr0.mabc` | Grammaire + automate LR(0) canonique (closure/goto) | **12 états exactement**, grammaire classique du dragon book (ch.4) |
| 2 | `src/slr1.mabc` | FIRST/FOLLOW + table SLR(1) (action/goto) + détection de conflits | FOLLOW sets corrects, 0 conflit, simulateur shift-reduce : 5/5 séquences de tokens correctement acceptées/rejetées |
| 3 | `src/codegen.mabc` | Format `.may`, construction automatique de la grammaire, génération de parseur autonome avec vraies actions `$$`/`$N` | Calculatrice complète (parenthèses, 4 opérations) générée, compilée séparément, exécutée : **résultats arithmétiques corrects** |
| 4 | `src/jcup.mabc` | Syntaxe inspirée de JCUP (`parser code`, `terminal`/`non terminal` typés, `start with`, `::=`, labels `SYM:label`, `RESULT`), `SemVal` typée personnalisable, productions epsilon, `%left`/`%right`/`%nonassoc`, intégration automatique lexer↔parser | Exemple JCUP exact de la demande (`ADD a b`), intégration bout en bout avec un vrai scanner matlex, précédence testée (`2+3*4=14`), epsilon testée (liste optionnelle sommée) |
| 5 | `src/idiomatic.mabc` | **Édition idiomatique** de tout le moteur : `bool`, `enum`+`match` (`ConflictDecision`, `ValKind`, `Assoc`), `for..in range` — même esprit que matlex rounds 8-9 | Tous les oracles précédents redonnent les mêmes résultats (`ADD 3 4`→7, epsilon→14, précédence→14, `calc.may`→13) après correction d'un bug trouvé pendant la conversion |
| 6 | `src/idiomatic.mabc` (suite) | `parser code { }` peut déclarer `struct`/`enum` (hissés au niveau racine) ; plus de `main()` dans les `.may` (convention JCUP) ; `enum sym {...}` remplace `#define` (comme le module CUP) ; exemple avec un vrai type composite (AST) | `ast_demo.may` : arbre `(2 + (3 * 4))` construit et évalué à `14` ; bug compilateur trouvé (coercion d'arguments de `new T(...)`) et corrigé |

## Pourquoi SLR(1) et pas LALR(1) complet

**SLR(1)** (Simple LR) réutilise l'automate LR(0) « brut » (closure/goto,
sans propagation de lookahead entre états) et décide des réductions via
les ensembles **FOLLOW** — nettement plus simple à implémenter
correctement que LALR(1) (qui demande de fusionner des états et de
propager des lookaheads d'état en état), tout en couvrant la grande
majorité des grammaires de langages de programmation usuelles
(expressions arithmétiques, structures de contrôle...). Les grammaires
qui ont besoin de LALR(1) mais pas de SLR(1) sont rares en pratique
(cas typiques : grammaires avec des règles très proches qui partagent
un préfixe et divergent tard).

---

## Round 1 — Grammaire + automate LR(0)

### Représentation

```matlabc
struct Sym { char[32] name; int is_terminal; }
struct Production { int lhs; int[8] rhs; int rhs_len; }
struct Grammar { Sym[32] symbols; int symbol_count; int start_symbol;
                 Production[32] prods; int prod_count; }

struct Item { int prod; int dot; }
struct ItemSet { Item[64] items; int item_count; }
```

- **`closure(g, iset)`** : point fixe classique — tant qu'un item
  `A -> a . B g` a un non-terminal `B` juste après le point, ajoute
  tous les `B -> . d` pour chaque règle de `B`.
- **`goto_set(g, iset, sym, out)`** : déplace le point sur `sym` pour
  chaque item concerné, puis referme le résultat.
- **`Automaton.build(g)`** : construit la collection canonique en
  partant de `closure({S' -> . S})`, avec une **égalité ensembliste**
  entre `ItemSet` (pas positionnelle — deux chemins différents dans
  `goto()` peuvent produire le même ensemble d'items dans un ordre
  d'insertion différent).

### Oracle de validation

Grammaire test = **exactement** celle du dragon book (ch. 4.6/4.7,
Aho/Ullman) :

```
0. S' -> E
1. E  -> E + T
2. E  -> T
3. T  -> T * F
4. T  -> F
5. F  -> ( E )
6. F  -> id
```

Son automate LR(0) canonique compte **exactement 12 états**, un
résultat bien connu — un oracle de correction immédiat, sans lequel un
bug de `closure`/`goto` pourrait facilement produire un nombre d'états
plausible mais faux. **Obtenu du premier coup.**

---

## Round 2 — FIRST/FOLLOW + table SLR(1)

### FIRST(X) — sans production vide

```
FIRST(terminal) = {terminal}
FIRST(non-terminal A) = union des FIRST(premier symbole du corps)
                         pour chaque regle de A
```

### FOLLOW(A)

```
$ dans FOLLOW(symbole de depart)
Pour chaque regle B -> ... A X ... :  FIRST(X) c FOLLOW(A)
Pour chaque regle B -> ... A (A en derniere position) :  FOLLOW(B) c FOLLOW(A)
```

**Limitation assumée** : pas de production vide (epsilon) — simplifie
le calcul (pas besoin de suivre la « nullabilité » d'un symbole). À
ajouter si un besoin réel se présente (listes optionnelles, etc).

### Table SLR(1) — encodage

`ACTION[état][terminal]` dans un seul entier :

| Valeur | Signification |
|---|---|
| `0` | erreur (aucune action) |
| `n > 0` | SHIFT vers l'état `n-1` |
| `n < 0`, `≠ ACCEPT_CODE` | REDUCE par la règle `-(n)-1` |
| `ACCEPT_CODE` (`-999999`) | ACCEPT |

Conflits résolus par convention yacc classique : **shift/reduce → SHIFT
retenu** ; **reduce/reduce → première règle déclarée retenue**.

### Validation : simulateur shift-reduce interne

Avant même de générer du code, la table est validée par un **driver
LR standard** (pile d'états uniquement, algorithme du dragon book
alg. 4.44) sur 5 séquences de tokens :

```
[OK] "id + id * id"              -> 1 (accepte)
[OK] "( id + id ) * id"          -> 1 (accepte)
[OK] "id + <fin>"                -> 0 (rejete, incomplet)
[OK] "id id"                     -> 0 (rejete, deux id consecutifs)
[OK] "( id  (parenthese non fermee)" -> 0 (rejete)
```

**5/5.**

---

## Round 3 — Format `.may` et génération de code

### Format (miroir du `.max` de matlex)

```
%token PLUS MINUS STAR SLASH LPAREN RPAREN NUM

%%
expr : expr PLUS term  { dollar_dollar = dollar_1 + dollar_3; }
     | expr MINUS term { dollar_dollar = dollar_1 - dollar_3; }
     | term             { dollar_dollar = dollar_1; }
     ;

term : term STAR factor  { dollar_dollar = dollar_1 * dollar_3; }
     | term SLASH factor { dollar_dollar = dollar_1 / dollar_3; }
     | factor             { dollar_dollar = dollar_1; }
     ;

factor : LPAREN expr RPAREN { dollar_dollar = dollar_2; }
       | NUM                 { dollar_dollar = dollar_1; }
       ;
%%

int main() { ... }
```

**`matyacc` est propriétaire de `%token`** — à la différence de matlex
(qui l'a retiré au round 7) : séparation des responsabilités classique
flex/bison — le lexer ne connaît que des entiers de type de token,
c'est le **parseur** qui nomme les types. Les tokens sont exposés comme
`#define NOM id` (grâce au préprocesseur ajouté plus tôt dans cette
session), consommables directement par les actions.

**La première règle déclarée fixe le symbole de départ** de la
grammaire (convention yacc/bison standard) — une règle augmentée
`S' -> depart` est ajoutée automatiquement.

### Actions sémantiques `$$` / `$1` / `$2` / ...

`rewrite_dollars()` réécrit `$$` en `dollar_dollar` et `$N` en
`dollar_N` au moment du **stockage** de la règle (pas à la génération),
conscient des chaînes/caractères littéraux et des commentaires — même
technique que `rewrite_action()` de matlex (round 6).

Le parseur généré déclare `dollar_1`, `dollar_2`, ... comme variables
locales lues depuis la pile de valeurs juste avant d'exécuter l'action
(déjà réécrite), puis pousse `dollar_dollar` sur la pile après avoir
dépilé les symboles consommés :

```matlabc
} else if (rule == 1) {
    int dollar_1 = self.vstack[self.vsp - 3 + 0];
    int dollar_2 = self.vstack[self.vsp - 3 + 1];
    int dollar_3 = self.vstack[self.vsp - 3 + 2];
    int dollar_dollar = 0;
    { dollar_dollar = dollar_1 + dollar_3; }
    self.sp = self.sp - 3;
    self.vsp = self.vsp - 3;
    ...
    self.vstack[self.vsp] = dollar_dollar;
    self.vsp = self.vsp + 1;
}
```

**Limitation assumée** : valeurs sémantiques limitées à des **entiers**
pour cette première version (`int vstack[]`) — pas de valeurs `str` ou
struct arbitraires. Extension naturelle pour un round ultérieur
(mirroir du `Symbol.ivalue`/`svalue` de matlex).

### Interface du parseur généré

```matlabc
int parse(int* tokens, int* values, int token_count)
```

Prend un tableau de tokens **déjà lexés** (`tokens[i]` = id de token,
`values[i]` = valeur sémantique entière associée) plutôt que d'appeler
un `yylex()` en direct — plus simple à générer et à tester
isolément. **Intégration avec un scanner matlex** : il suffit d'appeler
`sc.yylex()` en boucle pour remplir ces deux tableaux avant d'appeler
`parse()` — extension documentée, pas encore automatisée dans le code
généré (voir « Reste à faire »).

### Validation bout en bout : `spec/calc.may`

Calculatrice complète (parenthèses, `+ - * /`, précédence via structure
de grammaire à 3 niveaux `expr`/`term`/`factor`) : **7 tokens, 8 règles,
16 états LR(0), 0 conflit**. Générée, compilée **séparément**, exécutée :

```
( 3 + 4 ) * 2 - 5 / 5 = 13 (attendu 13)
2 + 3 * 4 = 14 (attendu 14)
```

Les deux résultats sont **arithmétiquement corrects**, precedence et
parenthèses respectées — preuve que le pipeline complet **spec → 
grammaire → automate LR(0) → FIRST/FOLLOW → table SLR(1) → code généré →
compilation indépendante → exécution avec vraie sémantique** fonctionne
de bout en bout.

---

## Bugs trouvés et corrigés pendant ce développement

### 1. Bug de compilateur — constante globale négative silencieusement mise à zéro

**Trouvé** : `const int ACCEPT_CODE = -999999;` valait **0** partout où
il était référencé — un test isolé l'a confirmé (`x == ACCEPT_CODE`
avec `x=0` évaluait `vrai`).

**Cause** : dans `codegen.ml`, l'initialisation d'une variable globale
**scalaire** (`Global`) gérait `EInt`/`EFloat`/`EBool`/etc. mais **pas**
`EUnop(Neg, EInt n)` — le nœud AST produit par un littéral **négatif**
(`-999999` se parse comme « moins unaire appliqué à 999999 », pas comme
un `EInt` directement négatif). Ce cas était déjà géré pour
l'initialisation des **lignes de matrices globales** (`EMatrix`) un peu
plus loin dans la même fonction, mais pas ici — l'initialisateur
retombait silencieusement sur `const_null` (zéro), sans aucune erreur
de compilation.

**Correctif** (`lib/backend/codegen.ml`) :

```diff
  | Some (Frontend.Ast.EBool (b, _)) -> const_int lt (if b then 1 else 0)
+ | Some (Frontend.Ast.EUnop (Frontend.Ast.Neg, Frontend.Ast.EInt (n, _), _)) ->
+     const_int lt (-n)
+ | Some (Frontend.Ast.EUnop (Frontend.Ast.Neg, Frontend.Ast.EFloat (f, _), _)) ->
+     const_float lt (-.f)
+ | Some (Frontend.Ast.EUnop (Frontend.Ast.Neg, Frontend.Ast.EDouble (f, _), _)) ->
+     const_float lt (-.f)
```

**Portée** : affectait **toute** constante globale **négative**
(`int`/`float`/`double`) dans un programme MATL-ABC — invisible sur les
342+122 tests existants (aucun n'avait de constante globale négative),
mais silencieux et trompeur (aucune erreur, juste une valeur fausse).
Régression vérifiée intacte (342 e2e + 122 unit) avant et après le
correctif.

### 2. Limitation trouvée (pas corrigée) — tableaux locaux de type struct

`Foo[12] arr;` comme **variable locale** échoue à la compilation
(`Erreur syntaxique`), alors que le **même type** comme **champ de
struct** (`Foo[12] arr;` dans `struct Bar { ... }`) fonctionne
parfaitement. Contournement systématique dans matyacc : tout tableau
« scratch » qui aurait naturellement été une variable locale
(`self.tmp_rhs` dans `MayParser`) est promu en **champ de la struct**
plutôt qu'en variable locale — même stratégie de contournement déjà
utilisée dans matlex (`RuleSpec`/`RuleFull` accumulés dans des champs,
jamais en locales). Documenté ici comme piste d'amélioration future du
compilateur, pas corrigé dans cette session (portée déjà très large).

---

## Intégration avec matlex

**Round 4 : automatisée** (voir section « Intégration automatique
lexer ↔ parser » ci-dessous) — `parse_with_scanner(sc)` pilote
`sc.yylex()` directement, plus besoin du pont manuel
`tokens[]`/`values[]` décrit ici pour le round 3. Cette section reste
comme trace historique de l'état round 3 :

matlex (round 7) génère des tokens via `#define NOM id` et une struct
`Symbol` (type + position + valeur) — le format que matyacc attendait
en entrée pour les valeurs sémantiques (`Symbol.ivalue`) au round 3,
et qu'il consomme désormais **directement** au round 4 via `SemVal` et
`pack_terminal()`.

---

## Round 4 — Syntaxe JCUP, `SemVal` typée, epsilon, précédence, intégration auto

### Changement de syntaxe : inspiré de JCUP, pas de bison/yacc classique

À la demande explicite, matyacc abandonne la syntaxe bison-like du round 3
au profit d'une syntaxe **inspirée de JCUP** (que l'utilisateur connaît
mieux que bison), adaptée aux idiomes MATL-ABC :

```
parser code {
    /* code MATL-ABC verbatim, injecte DANS le corps du parseur genere,
       comme des methodes -- meme principe que matlex round 7 */
}

terminal ADD;              /* terminal SANS valeur semantique */
terminal int NUMBER;       /* terminal AVEC valeur typee */

non terminal int expr;     /* non-terminal, TYPE OBLIGATOIRE */

start with expr;           /* symbole de depart EXPLICITE */

expr ::=
      ADD NUMBER:a NUMBER:b
      {
          RESULT = a + b;
          printf("Resultat = %d\n", RESULT);
      }
    ;
```

C'est **l'exemple exact fourni par l'utilisateur**, traduit en syntaxe
MATL-ABC — testé et vérifié : `RESULT = 7` pour `ADD 3 4`.

Mots-clés repris de JCUP, tels que demandés : `terminal`, `non terminal`,
`start with`, l'étiquetage positionnel `SYMBOLE:label`, et `RESULT` (au
lieu de `$$`). **Différence assumée avec JCUP pur** : un `%%` optionnel
permet, comme chez matlex/matyacc round 1-3, d'ajouter une section de code
utilisateur final (`main()`) après la grammaire — JCUP n'a pas cette
notion (tout vit dans `parser code { }`), mais c'est nécessaire ici pour
pouvoir tester le parseur généré directement.

**`RESULT` et les labels (`a`, `b`) sont des identifiants MATL-ABC
ORDINAIRES côté généré** — contrairement à `$$`/`$N` (round 3), aucune
réécriture textuelle n'est nécessaire : le générateur **déclare
simplement des variables locales portant exactement ces noms** avant de
coller l'action telle quelle. Plus simple et plus robuste que
`rewrite_dollars()`.

### `SemVal` — la personnalisation des « dollars » demandée

Fini le `int vstack[]` unique du round 3. Chaque terminal/non-terminal
porte désormais un **type déclaré**, qui détermine comment sa valeur est
stockée dans une petite union étiquetée :

```matlabc
struct SemVal {
    int  kind;    /* -1=aucune valeur, 0=int, 1=str, 2=pointeur generique */
    int  ival;
    str  sval;
    rawptr pval;
}
```

| Type déclaré | `kind` | Champ utilisé | Exemple |
|---|---|---|---|
| *(aucun)* | -1 | — | `terminal ADD;` (juste un jeton, aucune valeur) |
| `int` | 0 | `ival` | `terminal int NUMBER;` |
| `str` | 1 | `sval` | `terminal str ID;` |
| *n'importe quel autre nom* | 2 | `pval` (`cast<T*>(...)`) | `non terminal Node expr;` → struct/arbre/pile/matrice/tableau/**« un autre symbole »** — tout ce qui se manipule via pointeur en MATL-ABC |

C'est la réponse directe à « je veux changer pour que ce soit un
matrix/struct/tableau/pile/arbre/un autre symbole plutôt que toujours des
dollars » : matyacc ne connaît **aucun** de ces types à l'avance — il se
contente de re-caster un `rawptr` générique vers le type EXACT déclaré
par l'utilisateur dans sa spec, réalignant tout « sous le capot » via
`kind`.

### Productions epsilon (vides)

```
optlist ::=
      /* vide (epsilon) : liste optionnelle */
      { RESULT = 0; }
    | optlist:a NUM:n { RESULT = a + n; }
    ;
```

`closure()`/`goto()` (round 1) géraient déjà `rhs_len == 0` sans
modification (un item avec un corps vide est immédiatement « complet »).
Le vrai travail était dans **FIRST/FOLLOW**, réécrits pour suivre la
**nullabilité** de chaque symbole (`compute_nullable()`, point fixe
classique) et propager correctement à travers une chaîne de symboles
potentiellement tous nullables — plus seulement « le premier symbole du
corps » comme au round 2.

**Validé** : liste `[5, 7, 2]` construite récursivement depuis une
production vide → somme = 14 ; liste vide → 0.

### `%left` / `%right` / `%nonassoc`

```
%left PLUS MINUS;    /* precedence la plus BASSE (declaree en premier) */
%left STAR SLASH;    /* precedence plus HAUTE (declaree apres) */
```

Chaque terminal cité reçoit un niveau de précédence (croissant à chaque
**déclaration**, convention yacc standard) et une associativité. Une
règle hérite de la précédence de son **dernier terminal** (convention
yacc). `decide_shift_over_reduce()` compare précédences/associativités
pour trancher un conflit shift/reduce **avant** de retomber sur la
convention historique (SHIFT gagne) si l'une des deux précédences est
absente.

**Validé** sur la grammaire ambiguë classique
`expr ::= expr PLUS expr | expr STAR expr | NUM` (sans la structure en
couches `expr`/`term`/`factor` qui l'évitait au round 3) :
`2 + 3 * 4 = 14` — la précédence résout correctement les 4 conflits
shift/reduce structurels de cette grammaire.

### Intégration automatique lexer ↔ parser

**Demande satisfaite** : `parse_with_scanner(<ScannerType>* sc)`,
générée automatiquement à côté de `parse()`, pilote `sc.yylex()` **en
direct** — plus besoin de pré-tokéniser à la main :

```matlabc
GenScanner sc;
sc.init("ADD 3 4");

GenParser p;
p.init();

SemVal result = p.parse_with_scanner(&sc);   /* boucle interne : yylex() -> SemVal -> shift/reduce */
```

Suppose l'interface standard générée par matlex (`Symbol{sym_type, line,
column, ivalue, svalue}`, méthode `yylex()`) — configurable via une
nouvelle directive `%scanner NomDeStruct;` dans la spec `.may` si le nom
diffère du défaut `GenScanner`.

**Validé bout en bout, avec un VRAI scanner matlex** (pas un bouchon) :
spec matlex `spec/add_lexer.max` (tokenise `ADD`/nombres) + spec matyacc
`spec/add_parser.may` (grammaire `ADD NUMBER:a NUMBER:b`) → générés
séparément → **combinés dans un seul fichier** → compilés → exécutés sur
`"ADD 3 4"` :

```
=== integration automatique matlex <-> matyacc ===
ADD 3 4  =>  RESULT = 7 (attendu 7)
```

**Limitation assumée** : les *identifiants* de token doivent encore être
synchronisés **manuellement** entre les deux specs (le terminal `NUMBER`
déclaré en id 1 côté matyacc doit correspondre à `symbolInt(1, ...)` côté
matlex) — voir « Reste à faire ».

### Bugs trouvés (dans matyacc, pas le compilateur)

1. **Commentaires non ignorés dans la section des règles** : un
   commentaire `/* ... */` placé entre deux symboles d'une alternative
   était lu comme s'il s'agissait de noms de symboles RHS (`optlist ->
   vide epsilon liste optionnelle` au lieu de `optlist -> ` epsilon).
   Corrigé en donnant à `skip_ws_nl()` la même conscience des
   commentaires `/* */`/`//` que `rewrite_action()` (matlex) et
   `preprocess.ml` (compilateur) — appliqué **partout** où `skip_ws_nl`
   est utilisé (déclarations ET règles), pas seulement dans les actions.
2. **Chaîne `if/else if` mal fermée dans `pack_terminal()`** : même
   famille de bug rencontrée plusieurs fois dans ce projet (matlex round
   5, matyacc round 3) — chaque branche générée se refermait
   individuellement (`{ ... }`) au lieu de rester ouverte jusqu'à la
   branche suivante, cassant la compilation du code généré. Corrigé.

**Aucun nouveau bug de compilateur** ce round-ci — bon signe, les
correctifs des rounds précédents (fuite de pile, `strlen` sur
pointeur-vers-tableau, constante globale négative) couvrent
apparemment la plupart des angles morts rencontrés par ce style de
génération de code.

### "DDS" (Définitions Dirigées par la Syntaxe)

Le mécanisme `RESULT`/labels/`SemVal` **est** une implémentation de
Définition Dirigée par la Syntaxe (Syntax-Directed Definition) : chaque
production porte une règle sémantique (l'action) qui calcule la valeur
du non-terminal en tête à partir des valeurs de son corps — exactement
la définition formelle d'une DDS. Rien à ajouter algorithmiquement ici :
c'est la fondation même de `do_reduce()`.

### Génération de code et « optimisations »

Les actions étant du **code MATL-ABC arbitraire**, rien n'empêche d'y
émettre directement du code cible plutôt que de calculer une valeur —
par exemple, remplacer `RESULT = a + b;` par
`RESULT = emit_instruction("ADD", a, b);` pour construire une
représentation intermédiaire (liste d'instructions, arbre) au lieu d'un
résultat immédiat, exactement comme le ferait un vrai compilateur à ce
stade. Aucune infrastructure supplémentaire n'était nécessaire : c'est
une conséquence directe de « les actions sont du code MATL-ABC normal ».
Pas d'optimisation de table dédiée dans ce round (au-delà de la table
SLR(1) déjà compacte) — piste possible : compression de table (défaut
commun le plus fréquent factorisé), non implémentée, effet marginal sur
des grammaires de cette taille.

## Round 5 — Édition idiomatique

### Ce qui a été fait

Reprise de **tout le moteur** (`Sym`/`Production`/`Grammar`, `Item`/
`ItemSet`, `closure`/`goto_set`, `Automaton`, `SetTables`, `SLRTable`,
`JCupParser`) dans le même esprit que matlex rounds 8-9 :

- **`bool`** partout où une valeur est logiquement booléenne :
  `is_terminal` (champ de `Sym`), `contains()`/`add()`/`equals()` de
  `ItemSet` (retournent désormais `bool`), tous les drapeaux de
  contrôle de `JCupParser` (`done`, `alts_done`, `collecting`,
  `closed`, `in_str`, `in_chr`, `scanning`, `copying`), `processed[]`
  (`Automaton.build`), `nullable[]`/`first_set[]`/`follow_set[]`
  (`SetTables` — ces derniers sont des **bitsets d'appartenance à un
  ensemble**, un cas d'usage idéal pour `bool[]`).
- **`enum` + `match`** pour trois dispatches auparavant encodés en
  entiers magiques :
  - `enum ConflictDecision { PreferShift, PreferReduce, ForceError,
    NoPrecedence }` — `decide_shift_over_reduce()` et ses deux sites
    d'appel dans `SLRTable.build()`.
  - `enum ValKind { VNone, VInt, VStr, VOther }` — `classify_type()`
    et la génération de code dans `gen_parser_v2` (déclaration des
    locales `$label`, packing de `RESULT`, `pack_terminal()`).
  - `enum Assoc { AssocNone, AssocLeft, AssocRight, AssocNonassoc }` —
    précédence/associativité, à la place de `0`/`1`/`2`/`3` bruts.
- **`for i in range(n)`** pour toute boucle à borne statique connue à
  l'avance — la quasi-totalité des boucles du moteur (`closure`,
  `goto_set`, `Automaton.build`, `compute_nullable`/`compute_first`/
  `compute_follow`, `SLRTable.build`) s'y prêtaient, contrairement aux
  boucles à condition d'arrêt dynamique (points fixes `while(changed)`)
  qui restent `while` à dessein.

**Bonus inattendu** : plusieurs gabarits `printf` de `gen_parser_v2`
(qui **impriment** le code du parseur généré, pas le code de matyacc
lui-même) utilisaient aussi `int done = 0`/`while (done == 0)` — la
conversion regex globale les a *aussi* convertis en `bool done = false`/
`while (!done)`. Le code **généré** par matyacc est donc lui aussi plus
idiomatique, sans effort supplémentaire.

### Bug trouvé PENDANT la conversion (et corrigé)

En convertissant `compute_follow()` (`while (p < g.prod_count) { ...
p = p + 1; }` → `for p in range(g.prod_count) { ... }`), l'instruction
d'incrémentation manuelle `p = p + 1;`, devenue inutile avec le
`for-range`, n'a **pas été retirée** — laissée comme instruction morte
en fin de corps de boucle. Conséquence réelle et sérieuse : `p` se
retrouvait incrémenté **deux fois par itération** (une fois par le
`for-range` implicite, une fois par la ligne orpheline), sautant une
production sur deux dans le calcul de FOLLOW → des cases entières de
la table SLR(1) manquantes → un parseur qui **calcule un résultat
faux sans jamais signaler d'erreur** (`3 + 4` donnait `2` au lieu de
`7` sur la grammaire calculatrice en couches `expr`/`term`/`factor`,
alors que la grammaire à plat de l'exemple `ADD` fonctionnait
correctement par coïncidence — un seul niveau de règles, moins
sensible au bug).

**Détecté par comparaison directe** : le même fichier `.may` compilé
avec l'ancien moteur (`jcup.mabc`, round 4) donnait `7` ; avec le
nouveau (`idiomatic.mabc`, round 5), `2`. La divergence sur un test
qui passait auparavant a immédiatement pointé vers une régression de
CETTE session plutôt qu'un bug pré-existant — et une relecture
systématique de tous les `for i in range(...)` du fichier (recherche
de toute ligne `X = X + 1;` orpheline correspondant à la variable de
boucle) a confirmé qu'il s'agissait d'un cas **isolé** (une seule
occurrence sur ~25 boucles converties).

**Leçon méthodologique** : après toute conversion `while` → `for-range`
automatisée (script ou manuelle), vérifier systématiquement qu'aucune
incrémentation manuelle de la variable de boucle ne subsiste dans le
corps — un `grep` ciblé (`variable = variable + 1;` à l'intérieur d'un
bloc `for variable in range(...)`) suffit à l'détecter a posteriori.

### Découverte annexe : fichier de test obsolète

En testant, `spec/calc.may` (resté au format round 3 — `%token`,
`dollar_dollar`, pas de `terminal`/`non terminal`/`::=`) provoquait un
**segfault immédiat** — confirmé identique avec l'ancien ET le nouveau
moteur (pas une régression de cette session, juste un fichier jamais
migré vers la syntaxe JCUP du round 4). Corrigé en réécrivant
`calc.may` en syntaxe JCUP correcte. Noté comme piste d'amélioration :
matyacc devrait détecter une spec malformée (aucun `terminal`/`non
terminal` déclaré, par exemple) et échouer proprement avec un message
clair plutôt que de segfault sur un accès tableau hors bornes.

### Validation

Tous les oracles précédents redonnent le **même résultat** qu'avec le
moteur non-idiomatique (round 4), après correction du bug ci-dessus :

| Test | Round 4 (original) | Round 5 (idiomatique) |
|---|---|---|
| `ADD 3 4` (exemple JCUP exact) | `RESULT = 7` | `RESULT = 7` |
| Intégration scanner matlex réel | `RESULT = 7` | `RESULT = 7` |
| Liste epsilon `[5,7,2]` | somme `14` | somme `14` |
| Précédence `2 + 3 * 4` | `14` | `14` |
| `calc.may` (corrigé) `(3+4)*2-5/5` | `13` | `13` |

## Round 6 — `parser code` composite, `enum sym`, exemple AST

### 1. `parser code { }` peut désormais déclarer des `struct`/`enum`

**Limitation trouvée** : tout le contenu de `parser code { }` était
collé **tel quel comme méthodes** à l'intérieur de la struct générée
(`GenParser`) — impossible d'y écrire `struct Node { ... }`, puisque
MATL-ABC n'autorise pas une déclaration de type **imbriquée** dans une
autre struct.

**Correctif** : `split_top_level_decls()` post-traite le texte capturé
de `parser code { }` et **hisse** chaque bloc `struct NOM { ... }` ou
`enum NOM { ... }` (repéré par bordure de mot + comptage d'accolades
équilibré, conscient chaînes/caractères/commentaires) vers un nouveau
buffer `top_level_code`, imprimé **avant** `SemVal`/`GenParser` dans le
fichier généré. Tout le reste (méthodes, code ordinaire) continue
d'être collé comme méthodes de `GenParser`, comme avant :

```
parser code {
    struct Node {
        int   op;
        int   value;
        Node* left;
        Node* right;
        void init(int o, int v, Node* l, Node* r) { ... }
    }

    int eval_node(Node* n) { ... }   /* reste une methode de GenParser */
}
```

`Node` est désormais un vrai type top-level ; `eval_node` reste une
méthode (`self.eval_node(...)`), les deux coexistent naturellement.

### 2. Convention : pas de `main()` dans le `.may`

Corrigé sur tous les exemples (`calc.may`, `ast_demo.may`, etc.) : la
spec `.may` ne contient plus que la **grammaire pure** (terminaux,
non-terminaux, règles), comme le veut la convention JCUP/bison
standard. Le pilote de test (`main()`, construction de la séquence de
tokens, appel à `parse()`) vit désormais dans un **fichier séparé**,
qui `#include` implicitement le parseur généré en le concaténant.
L'extension `%% code final` (héritée des rounds 1-3) reste
**disponible** pour qui veut vraiment embarquer du code dans le
`.may`, mais n'est plus utilisée dans les exemples livrés.

### 3. `enum sym { ... }` remplace les `#define`, comme le module CUP

**Demande** : éviter les entiers magiques (`symbol(0)`, `symbol(1)`)
côté matlex, qui obligent à faire correspondre manuellement l'ordre
des `symbol(N)` à l'ordre des `terminal` déclarés côté matyacc — source
d'erreur silencieuse si l'ordre change d'un côté sans l'autre.

**Avant (round 5)** :
```matlabc
#define ADD 0
#define NUMBER 1
```
**Maintenant (round 6)**, à la manière du module `sym` généré par CUP :
```matlabc
enum sym { ADD, NUMBER }
```

Côté matlex, `spec/add_lexer.max` référence désormais `sym.ADD`/
`sym.NUMBER` au lieu de `0`/`1` :
```matlabc
ADD        { return symbol(sym.ADD); }
{CHIFFRE}+ { return symbolInt(sym.NUMBER, atoi(yytext().data())); }
```

**Contrainte d'ordre découverte en testant** : `enum sym` doit être
**visible avant** son utilisation — le fichier généré par **matyacc**
(qui définit `enum sym`) doit donc être concaténé **avant** celui
généré par **matlex** (qui l'utilise) dans l'unité de compilation
finale. Documenté et respecté dans `idiomatic_integration_demo.mabc`.
Reste **manuel** (voir « Reste à faire ») : le *nombre* et le *nom* des
tokens ; seule la fragilité des *valeurs numériques* est éliminée.

### 4. Exemple avec capacités composites : construction d'un AST

`spec/ast_demo.may` — `non terminal Node expr;` (pas `int` !) :
chaque réduction construit un vrai nœud d'arbre via `new Node(...)`
plutôt que de calculer une valeur immédiatement :

```matlabc
expr ::=
      expr:a PLUS expr:b { RESULT = new Node(1, 0, a, b); }
    | expr:a STAR expr:b { RESULT = new Node(2, 0, a, b); }
    | NUM:n                { RESULT = new Node(0, n, null, null); }
    ;
```

Le code généré déclare correctement `Node* a`/`Node* b`/`Node* RESULT`
(via `cast<Node*>(...)`/`SemVal.pval`), exactement la promesse du
round 4 (« personnaliser les dollars ») mise en pratique avec un
**vrai type composite**, pas juste `int`/`str`. Validé sur `"2 + 3 * 4"` :

```
arbre : (2 + (3 * 4))
evaluation : 14 (attendu 14, 2 + 3*4)
```

L'arbre est correctement construit (parenthésage refletant la
précédence), traversé récursivement (`eval_node`) et affiché
(`print_node`) — les deux étant des méthodes définies dans `parser
code { }`.

### Bug de compilateur trouvé et corrigé

**`new T(...)` avec un argument `null` passé à un paramètre de type
pointeur de struct spécifique** (`new Node(0, n, null, null)`, où
`init()` attend `Node* l, Node* r`) provoquait une erreur de type LLVM
à l'émission (*"defined with type ... but expected ..."*).

**Cause** : `ENull` est toujours codegen'é comme un pointeur générique,
**sans connaissance du type de paramètre attendu** au site d'appel. Les
arguments de `new T(...)` étaient passés à `init()` **sans coercion**
vers le type exact attendu — contrairement aux appels de méthode
ordinaires, qui avaient déjà reçu ce correctif au round 12 (documenté
dans `lib/backend/codegen.ml`).

**Correctif** : appliquer la **même coercion d'arguments** (déjà
utilisée pour les appels de méthode) au site d'appel du constructeur
`init()` de `new T(...)` — caste chaque argument vers le type de
paramètre exact via `codegen_cast`.

**Portée** : affecte tout `new T(arg1, ..., argN)` où au moins un
argument est `null` (ou plus généralement une expression dont le type
LLVM généré ne correspond pas exactement au type déclaré du paramètre)
et où le paramètre correspondant est un pointeur de struct spécifique
— un cas plausible dans n'importe quel programme MATL-ABC construisant
des structures chaînées (listes, arbres) via `new`, invisible sur les
342 tests existants (aucun n'avait cette combinaison précise). Régression
vérifiée intacte avant et après (342 e2e + 122 unit).

## Reste à faire

- **LALR(1) complet** (fusion d'états LR(1), propagation de lookahead) —
  délibérément **non tenté** ce round : la complexité d'une implémentation
  correcte (items LR(1) avec lookahead, construction canonique PUIS
  fusion des états de même cœur LR(0), avec le risque classique des
  conflits reduce/reduce introduits par la fusion) est disproportionnée
  par rapport au gain pour les grammaires ciblées ici — SLR(1) + précédence
  couvre déjà calculatrices, listes epsilon, et grammaires ambiguës
  classiques. À reconsidérer si une grammaire réelle s'avère nécessiter
  strictement LALR(1).
- **Synchronisation automatique des NOMS de token** entre matlex et
  matyacc (round 6 a résolu la fragilité des *valeurs numériques*
  via `enum sym` — reste manuel : matlex doit encore connaître les
  *noms* exacts déclarés côté matyacc, et l'ORDRE de concaténation des
  deux fichiers générés doit respecter `enum sym` avant son usage,
  voir round 6 ci-dessus. Idéalement, un futur outil pourrait générer
  un fichier `sym.mabc` unique, importé par les deux générateurs).
- **États lexicaux** matlex (`yybegin`/`yystate` existent mais ne
  filtrent pas encore les règles actives — round 6 de matlex).
- **Robustesse face aux specs malformées** : une spec `.may` ou `.max`
  sans déclarations suffisantes devrait échouer proprement (message
  d'erreur) plutôt que de risquer un accès hors bornes — voir la
  découverte du fichier `calc.may` obsolète ci-dessus.
- **Bibliothèque partagée (« shared memory library »)** pour qu'un lexer
  et son parser partagent un même processus/état sans recompilation
  conjointe — explicitement reporté à plus tard par l'utilisateur.

## Régression

Vérifiée intacte après les 5 rounds et tous les correctifs :
**342/342 tests e2e, 122/122 tests unitaires**.
