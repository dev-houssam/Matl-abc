# matlex — générateur de lexer pour MATL-ABC

Équivalent de `flex`/`JFlex` : compile des expressions régulières (une ou
plusieurs règles nommées) en un automate déterministe, puis scanne un texte
source en émettant des tokens par « plus longue correspondance » (maximal
munch), exactement comme le ferait un scanner généré par flex.

Écrit **en MATL-ABC lui-même** — ce projet, avec `matyacc` (générateur de
parser, à venir), sert de test de charge pour le langage : compilateurs de
petite taille, mais avec de vrais algorithmes non triviaux (construction de
Thompson, subset construction, scan par automate).

Le projet avance par « rounds » testés indépendamment ; chaque round est un
fichier `.mabc` autonome et compilable dans `src/`.

## Sommaire des rounds

| Round | Fichier | Contenu | Tests |
|---|---|---|---|
| 1 | `src/nfa.mabc` | Regex → NFA (construction de Thompson) : littéraux, `|`, concaténation, `()`, `* + ?`, échappement `\` | 28/28 |
| 2 | `src/dfa.mabc` | + classes `[abc]`, `[a-z]`, `[^...]`, `.` (any) ; NFA → DFA (subset construction) | 41/41 |
| 3 | `src/scanner.mabc` | NFA multi-règles + scanner maximal munch (vrai tokenizer multi-tokens) | 4/4 scénarios |
| 4 | `src/codegen.mabc` | Minimisation DFA, spec texte `.mlex`, génération de scanner MATL-ABC autonome | 4+3+2 scénarios + génération vérifiée par compilation séparée |
| 5 | `src/flex.mabc` | Format `.max` quasi-conforme flex (`%{ %}`/`%token`/actions réelles), scanner `yylex()`, entrée fichier/stdin/interactif, fichier de tokens pour matyacc | Vérifié bout en bout sur une vraie spec `.max` (MiniCalc, 11 tokens, 12 règles) |
| 6 | `src/jflex.mabc` | Définitions nommées `{NOM}`, comptage d'accolades robuste, API JFlex complète (`yytext()`, `yyline`, `yycolumn`, `yybegin`/`yystate`, `yypushback`, `yyreset`, `%struct`, `%unicode`, `%line`, `%column`) | Vérifié bout en bout sur MiniCalc avec définitions imbriquées (4 définitions, 12 règles) |
| 7 | `src/symbol.mabc` | Retrait de `%token` (rôle de matyacc) ; struct `Symbol` générée (type/ligne/colonne/valeur) ; prologue `%{ %}` injecté DANS le scanner comme méthodes | Vérifié bout en bout sur MiniCalc3 (13 règles, mots-clés, valeurs `ivalue`/`svalue`, gestion d'erreur) |
| 8 | `src/idiomatic.mabc` | **Édition idiomatique** du moteur (Frag/NFA/RegexParser/DFA/DfaBuilder/DfaMinimizer) : `bool`, `enum`+`match`, `for..in range`, `str` native — remplace l'esprit « C avec une autre syntaxe » des rounds 1-7 | **31/31**, résultats identiques aux oracles des rounds 1-2 |
| 9 | `src/idiomatic_scanner.mabc` | **Édition idiomatique** de la couche `.max`/JFlex (`MaxParser`, `rewrite_action`, génération de scanner `GenScanner`/`Symbol`), bâtie sur le moteur du round 8 | Résultats identiques au round 7 sur la même spec de démo **et** sur `spec/add_lexer.max` (prologue avec fonctions utilitaires, tokenisation `ADD 3 4`) |

Chaque fichier est **autoportant** (copie complète des structures des rounds
précédents, augmentées) plutôt que de dépendre d'un système de modules
multi-fichiers — MATL-ABC ne permet de compiler qu'un seul fichier source par
`conf.toml` (`entry = "..."`), un import inter-`.mabc` n'existe pas
(seuls les headers `.mabh` sont importables, pour du binding C/FFI).

---

## Round 1 — Regex → NFA (construction de Thompson)

### Grammaire supportée

```
union    := concat ('|' concat)*
concat   := repeat+
repeat   := atom ('*' | '+' | '?')?
atom     := '(' union ')' | '\' any_char | literal_char
```

### Architecture

- **`NFA`** : arène à capacité fixe (états + transitions en tableaux avec
  compteur), pas d'allocation dynamique de listes — idiome cohérent avec le
  reste du compilateur MATL-ABC (cf. les structs à capacité bornée du
  compilateur lui-même, ex. table de symboles à 16 entrées).
- **`Frag`** : fragment de Thompson `{start, accept}`, retourné **par
  valeur** par les constructeurs `frag_char`, `frag_concat`, `frag_union`,
  `frag_star`, `frag_plus`, `frag_optional`.
- **`RegexParser`** : descente récursive sur le pattern, construit le NFA
  directement (pas d'AST regex intermédiaire) — chaque règle grammaticale
  émet son fragment Thompson au fur et à mesure du parsing.
- **`NFA.matches(str)`** : simulation par ensembles d'états (tableau
  d'appartenance `int[N]`, pas de bitset), avec calcul de clôture epsilon
  par point fixe (`eps_closure`).

### Exemple

```matlabc
NFA nfa;
nfa.init();
RegexParser p;
p.init("(a|b)*c", &nfa);
p.compile();
int ok = nfa.matches("aabbabc");   // -> 1
```

---

## Round 2 — Classes de caractères, `.`, puis NFA → DFA

### Nouveautés syntaxe regex

- `.` — n'importe quel caractère
- `[abc]` — classe (union de caractères)
- `[a-z]` — plage dans une classe
- `[^...]` — classe négée

### Représentation des transitions

Le round 1 utilisait un booléen `trans_is_eps`. Le round 2 le remplace par
`trans_kind` (int) :

| kind | signification |
|---|---|
| 0 | caractère littéral (`trans_sym`) |
| 1 | epsilon |
| 2 | any (`.`) |
| 3 | classe (`trans_class` → index dans les tables de classes) |

**Choix de conception important** : une classe n'est **pas** déroulée en
union de fragments Thompson (ce qui exploserait le nombre d'états pour une
classe large comme `[a-zA-Z]`, 52 caractères). Une classe est stockée à part
sur le NFA (`class_neg`, `class_range_count`, `class_lo`, `class_hi` — jusqu'à
16 sous-plages par classe) et vérifiée **par prédicat** au moment du matching
(`char_in_class`), via une seule transition `kind=3` par classe.

### NFA → DFA (subset construction)

- **`DFA`** : automate déterministe à capacité fixe — 128 états max, table
  de transition aplatie `trans[état*256 + octet] -> état ou -1`.
- **`DfaBuilder`** : subset construction classique. Chaque état DFA
  correspond à un sous-ensemble d'états NFA (après clôture epsilon),
  représenté comme une **liste triée d'ids** (et non un bitset complet) —
  économie mémoire, et comparaison d'égalité en O(longueur du sous-ensemble)
  plutôt que O(nombre d'états NFA).
- Worklist simple (balayage linéaire des états DFA non traités) plutôt
  qu'une vraie file — suffisant à cette échelle, à revoir si le nombre
  d'états DFA devient significatif.

### Exemple

```matlabc
NFA nfa; nfa.init();
RegexParser p; p.init("[a-zA-Z_][a-zA-Z0-9_]*", &nfa); p.compile();

DFA dfa; dfa.init();
DfaBuilder b; b.init(&nfa, &dfa); b.build();

int ok = dfa.matches("variable_1");   // -> 1, en O(longueur), sans cloture epsilon
```

### ⚠️ Bug de compilateur majeur trouvé et corrigé pendant ce round

**Symptôme** : `DfaBuilder.build()` segfaultait après quelques centaines
d'itérations de sa boucle principale (balayage des 256 valeurs d'octet par
état DFA), sans rapport apparent entre la ligne où le crash survenait et une
quelconque logique métier — le crash se déplaçait d'une exécution à l'autre.

**Cause réelle** (confirmée par calcul d'adresses sous `gdb` : RSP se
trouvait à 7,75 Mo de RBP au moment du crash, juste sous la limite de pile
par défaut de 8 Mo) : toute déclaration de variable locale
(`int[4096] moveset;` notamment) à l'intérieur d'un corps de boucle générait
son instruction LLVM `alloca` **au point d'insertion courant** — c'est-à-dire
dans le basic block de la boucle elle-même — au lieu d'être placée une seule
fois dans le bloc d'entrée de la fonction. Or en LLVM, une `alloca` qui
s'exécute plusieurs fois réserve une **nouvelle** zone de pile à chaque
passage, sans jamais libérer les précédentes (la pile ne se dépile qu'au
retour de la fonction). Un tableau de 16 Ko déclaré dans une boucle de
quelques centaines d'itérations épuisait ainsi la pile.

**Correctif** (`lib/backend/codegen.ml`) : ajout d'une fonction
`build_entry_alloca` qui positionne systématiquement l'`alloca` au tout
début du bloc d'entrée de la fonction courante (récupéré via
`block_parent` + `entry_block` + `builder_at (instr_begin entry)`), quel que
soit l'endroit du corps où la variable est déclarée — exactement le
comportement de tout compilateur C sérieux. `SDecl` utilise désormais cette
fonction au lieu de `build_alloca cg.builder` directement.

Régression vérifiée intacte : 342 tests e2e + 122 tests unitaires du
compilateur, avant et après le correctif.

C'est le genre de bug invisible sur du code « petit » (aucun des 342+122
tests existants ne le déclenchait) mais bloquant dès qu'un vrai algorithme
avec boucles non triviales est écrit — exactement ce que ce projet matlex est
censé débusquer.

---

## Round 3 — NFA multi-règles + scanner « maximal munch »

### Ce qui change

- **`NFA.accept_rule[état]`** (int, -1 par défaut) : chaque état acceptant
  porte désormais un **id de règle**. Un seul NFA peut ainsi représenter
  *plusieurs* règles simultanément : un unique état `start` commun, relié
  par epsilon au fragment Thompson de chaque règle
  (`RegexParser.compile_rule(rule_id)`, qui remplace `compile()` pour ce
  cas d'usage).
- **`DFA.is_final[état]`** ne stocke plus un booléen mais l'**id de la règle
  gagnante** pour cet état (-1 si aucune règle n'accepte). Quand plusieurs
  règles NFA acceptent simultanément dans un même sous-ensemble DFA
  (ambiguïté), la règle de **plus petit id l'emporte** — exactement la
  sémantique flex/JFlex : *« en cas d'égalité de longueur, la règle déclarée
  en premier gagne »*. Le calcul se fait dans `DfaBuilder.find_or_add`.
- **`Scanner`** : structure `{dfa, src, pos, len}`. `next_token()` implémente
  le maximal munch : on avance dans le DFA tant que des transitions
  existent, en mémorisant la **dernière** position où l'automate était dans
  un état acceptant (et quelle règle). Dès qu'on ne peut plus avancer
  (`dfa.step(...) == -1`), on retombe sur cette dernière position connue et
  on émet un token pour la règle qui y était gagnante. Si aucune règle
  n'accepte, même sur un seul caractère : token d'erreur (`rule_id = -2`),
  et le scanner avance de 1 caractère pour permettre une reprise (stratégie
  de récupération standard).
- **`Scanner.next_real_token()`** : variante qui saute silencieusement les
  tokens de la règle 0 (réservée par convention à `WHITESPACE` dans le
  harnais de test) — modélise le pipeline réel *lexer → parser*, où
  l'espace/les commentaires ne sont jamais transmis au parser.

### Langage de démonstration (« MiniCalc »)

12 règles, construites par `build_minicalc_nfa()` :

| id | nom | pattern |
|---|---|---|
| 0 | WS | `[ \t\n\r]+` |
| 1 | IDENT | `[a-zA-Z_][a-zA-Z0-9_]*` |
| 2 | FLOAT | `[0-9]+\.[0-9]+` |
| 3 | INT | `[0-9]+` |
| 4 | PLUS | `\+` |
| 5 | MINUS | `-` |
| 6 | STAR | `\*` |
| 7 | SLASH | `/` |
| 8 | LPAREN | `\(` |
| 9 | RPAREN | `\)` |
| 10 | ASSIGN | `=` |
| 11 | SEMI | `;` |

Note : FLOAT est déclaré **avant** INT, mais l'ordre n'a en réalité aucune
importance ici — le maximal munch choisit de toute façon FLOAT sur
`"3.14"` car il consomme 4 caractères contre 1 seul pour INT (`"3"` puis
échec sur `.`). L'ordre ne compterait que si deux règles matchaient une
correspondance de **longueur strictement égale** sur la même entrée.

### Exemple

```matlabc
NFA nfa;
build_minicalc_nfa(&nfa);

DFA dfa; dfa.init();
DfaBuilder b; b.init(&nfa, &dfa); b.build();

Scanner sc;
sc.init(&dfa, "x = 3 + 4;");

while (sc.at_end() == 0) {
    TokenSpan tok = sc.next_real_token();
    // tok.rule_id, tok.start, tok.len
}
// -> IDENT('x') ASSIGN('=') INT('3') PLUS('+') INT('4') SEMI(';')
```

### Limitations connues (documentées, pas des bugs)

- Octets non-ASCII (>127) non gérés par `DFA.step` (retourne -1) : `char`
  est signé dans ce compilateur, un cast d'un octet ≥128 donne une valeur
  négative. Sans incidence pour un langage source ASCII classique.
- Pas d'ancres `^`/`$` : sans objet dans le modèle « plein texte / maximal
  munch » actuel (elles auraient un sens pour un mode « recherche dans une
  ligne », pas implémenté).
- Pas de quantificateur `{n,m}`.
- Pas de minimisation du DFA (Hopcroft/Moore) : les DFA produits sont
  corrects mais pas nécessairement minimaux en nombre d'états.
- Pas encore de format de spécification textuel `.mlex` externe (sections
  définitions/règles/actions à la flex) : les règles sont pour l'instant
  déclarées directement en code MATL-ABC (`compile_rule`).

---

## Round 4 — Minimisation DFA, spec texte `.mlex`, génération de code

### Minimisation DFA (`DfaMinimizer`)

Raffinement de partitions (variante simple de Moore/Hopcroft, adaptée à des
automates de petite taille — `O(itérations × états² × 256)` au lieu du
`O(n log n)` de Hopcroft, largement suffisant ici) :

1. **Partition initiale** : un groupe par valeur `is_final` *distincte* —
   **pas** un simple "final / non-final" comme la minimisation classique.
   Deux états acceptant des règles *différentes* ne sont jamais fusionnés,
   sinon on perdrait l'information de quel token émettre.
2. **Raffinement itératif** : un groupe se scinde dès que deux de ses états
   mènent, pour un même octet, vers des groupes différents. On répète
   jusqu'à point fixe.
3. **Reconstruction** : un état représentant par groupe final.

Sur la grammaire MiniCalc (12 règles), le DFA passe de 15 à 14 états — gain
modeste ici (peu de redondance structurelle dans une grammaire aussi
simple), mais le mécanisme est généralisable. Vérifié par équivalence
stricte de tokenisation (`test_scan_equiv`) entre DFA original et minimisé
sur plusieurs textes.

### Spec texte `.mlex` (`SpecParser`)

Format simplifié, une règle par ligne :

```
# commentaire
NOM     regex
```

Lignes vides et commentaires (`#`) ignorés. L'id de règle = ordre de
déclaration. **Pas encore** de sections `%{ %}` ni d'actions arbitraires à
la flex — prévu pour un round ultérieur si le besoin se confirme.

`SpecParser.build_nfa(nfa)` compile toutes les règles de la spec dans un NFA
combiné, exactement comme `build_minicalc_nfa()` le faisait « à la main » au
round 3. Vérifié par équivalence stricte avec la version codée en dur.

#### Piège rencontré : échappements `\t \n \r`

Écrire le pattern `WS` de la spec directement comme
`"[ \t\n\r]+"` dans une chaîne MATL-ABC hôte produit de **vrais octets de
contrôle** (tabulation, retour à la ligne réels) — ce qui casse le
découpage ligne-par-ligne de `SpecParser` (un `\n` réel est indiscernable
d'un saut de ligne de la spec elle-même). Deux correctifs complémentaires :

1. Dans la chaîne MATL-ABC hôte, doubler l'anti-slash (`"\\t\\n\\r"`) pour
   obtenir le **texte littéral** à deux caractères `\t`, `\n`, `\r` dans la
   spec (pas de vrais octets de contrôle).
2. `RegexParser` gagne une méthode `resolve_escape()` qui traduit *lui-même*
   `\t`/`\n`/`\r` vers les octets 9/10/13 au moment de compiler la regex
   (comme le fait tout moteur regex sérieux — jusque-là, round 1-3
   traitaient `\c` comme littéralement `c`, quel que soit `c`).

Aucun bug de compilateur ici — une erreur de conception dans le format de
test, corrigée au niveau de matlex lui-même.

### Génération de code (`gen_scanner`)

Émet sur `stdout` un programme MATL-ABC **complet et autonome** (aucune
dépendance à `NFA`/`RegexParser`/`DfaBuilder`/`DfaMinimizer`/`SpecParser` à
l'exécution) — exactement ce que produit `flex` (`.c`) ou `JFlex`
(`.java`) : le texte imprimé *est* le fichier `.mabc` à compiler.

Structure générée : `GenScanner` avec table de transition et `is_final`
« en dur » (tableaux remplis par affectations explicites dans `init()`,
MATL-ABC n'ayant pas de syntaxe de littéral de tableau), plus
`next_token()`/`next_real_token()` (même logique de maximal munch que le
`Scanner` du round 3), plus une fonction `gen_rule_name()` et un `main()`
de démonstration.

**Preuve d'autonomie** : le code généré a été extrait dans
`src/generated_scanner.mabc`, compilé **séparément** (sans passer par
matlex), exécuté, et produit une tokenisation strictement identique à
celle du scanner d'origine :

```
=== Scanner genere : tokenisation de 'x = 3 + 4;' ===
  [ 0: 1] IDENT
  [ 2: 1] ASSIGN
  [ 4: 1] INT
  [ 6: 1] PLUS
  [ 8: 1] INT
  [ 9: 1] SEMI
```

C'est la validation de bout en bout du pipeline complet :
**spec texte → NFA → DFA → minimisation → code MATL-ABC généré → compilation
indépendante → exécution correcte.**

### Aucun nouveau bug de compilateur

Signe supplémentaire que le fix du round 2 (fuite de pile) était bien la
cause racine de fragilité : malgré des boucles imbriquées à 4-5 niveaux
(minimisation notamment : boucle de convergence × états × représentants ×
256 octets), aucun bug de compilateur n'a été trouvé pendant ce round.

---

## Round 5 — Format `.max` quasi-conforme flex

matlex devient un vrai outil utilisable en pratique : l'utilisateur écrit un
fichier `.max` (ou `.l`), le passe à matlex (`matlex spec.max > lexer.mabc`
en pratique — ici : redirection depuis les marqueurs `stdout`, voir plus
bas), obtient un scanner MATL-ABC **autonome**, le compile, et l'exécute
sur du texte réel — fichier, `stdin` avec sentinelle, ou saisie interactive.

### Format `.max`

```
%{
    code MATL-ABC verbatim (inclus tel quel en tete du fichier genere)
%}

%token NOM1 NOM2 NOM3 ...     (peut apparaitre plusieurs fois)

%%
regex1     { action1 }
regex2     { action2 }
...
%%

code MATL-ABC utilisateur final (typiquement main()), inclus verbatim
```

Exemple réel (`spec/minicalc.max`, livré avec le projet) :

```
%{
/* Prologue : rien de special pour ce petit langage MiniCalc. */
%}

%token TOK_IDENT TOK_INT TOK_FLOAT TOK_PLUS TOK_MINUS TOK_STAR TOK_SLASH
%token TOK_LPAREN TOK_RPAREN TOK_ASSIGN TOK_SEMI

%%
[ \t\n\r]+               { int skip = 0; }
[a-zA-Z_][a-zA-Z0-9_]*   { return TOK_IDENT(); }
[0-9]+\.[0-9]+           { return TOK_FLOAT(); }
[0-9]+                   { return TOK_INT(); }
\+                       { return TOK_PLUS(); }
...
%%
```

### Différences assumées avec flex/JFlex

- **Pas de section « définitions » nommées** (`{NOM}` réutilisable dans les
  règles) — prévu pour un round ultérieur si le besoin se confirme.
- **Les tokens `%token` sont exposés comme des fonctions à zéro argument**
  (`TOK_INT()`, pas `TOK_INT`) : MATL-ABC n'a pas de macro `#define`, et une
  constante globale mutable demanderait `@perm` (mémoire partagée
  inter-processus, hors de propos ici). L'action écrit donc
  `return TOK_INT();` plutôt que `return TOK_INT;`.
- **`yylex()` retourne `-1` pour EOF**, pas `0` — `0` reste un id de token
  valide (le premier `%token` déclaré).
- **Aucun bloc d'action ne peut être vide** (`{ }` ou `{ /* commentaire */ }`
  seuls) : limitation du parseur MATL-ABC déjà rencontrée aux rounds 1 et 4
  (`if {} else {}` refusé). Une règle qui ignore son match (espaces,
  commentaires...) doit contenir au moins une instruction réelle, même
  triviale : `{ int skip = 0; }`.

### `MaxParser` : analyse du fichier `.max`

Découpage en 3 sections par les lignes `%%` :

1. **Prologue** : blocs `%{ ... %}` copiés **verbatim** en tête du fichier
   généré (imports supplémentaires, types auxiliaires...) ; lignes
   `%token NOM1 NOM2 ...` collectées dans `MaxFile.tokens[]`.
2. **Règles** : pour chaque règle, le *pattern* est tout ce qui précède la
   première `{` (espaces de fin retirés), l'*action* est extraite par
   **comptage d'accolades équilibrées** à partir de cette `{` — supporte
   des actions multi-lignes avec accolades imbriquées. (Limitation
   documentée : des accolades à l'intérieur d'une chaîne littérale dans
   l'action fausseraient ce comptage — cas non géré pour l'instant.)
3. **Épilogue** : tout le reste du fichier, copié **verbatim** à la fin du
   fichier généré (typiquement le `main()` de l'utilisateur). Si l'épilogue
   ne contient que des espaces/retours à la ligne (aucun caractère
   significatif), matlex considère qu'aucun code utilisateur n'a été fourni
   et génère son **driver par défaut** à la place.

### Génération de code v2 : dispatch d'action réelle (`yylex()`)

Contrairement au round 4 (qui renvoyait un simple id de règle numérique),
le scanner généré exécute maintenant le **vrai code d'action** de chaque
règle — exactement comme le `.c` généré par flex :

```matlabc
int yylex() {
    int cont = 1;
    int result = -1;
    while (cont == 1) {
        if (self.at_end()) {
            cont = 0;
        } else {
            char* yytext = &self.yytext_buf;
            int rid = self.scan_one();
            int yyleng = self.yyleng;
            if (rid == -1) {
                int lex_err = 1;              /* erreur lexicale, ignoree */
            } else if (rid == 0) { int skip = 0; }
            else if (rid == 1) { return TOK_IDENT(); }
            else if (rid == 2) { return TOK_INT(); }
            /* ... une branche par regle, dans l'ordre de declaration */
        }
    }
    return result;
}
```

Une action qui fait `return X;` **termine `yylex()` immédiatement** avec ce
token (le `return` inline sort directement de la fonction). Une action qui
ne fait *pas* de `return` (ex: ignorer les espaces) laisse la boucle
continuer sur le token suivant — comportement identique à flex. `yytext`
(le lexème matché, `char*` null-terminé) et `yyleng` (sa longueur) sont
disponibles dans le corps de chaque action.

### Driver par défaut (3 modes d'entrée)

Si l'utilisateur ne fournit pas son propre `main()` en épilogue, matlex en
génère un qui supporte :

1. **Fichier** : `./scanner chemin/vers/fichier` — lu entièrement via
   `fopen`/`fgets` en boucle (buffer 64 Ko).
2. **`stdin` avec sentinelle `EOF`** : `./scanner <<EOF ... EOF` (ou tout
   flux redirigé se terminant par une ligne contenant exactement `EOF`) —
   permet de délimiter l'entrée sans dépendre de la fermeture réelle du
   flux.
3. **`stdin` interactif / pipe simple** : `echo "..." | ./scanner` — lu
   jusqu'à la fin réelle du flux (`getchar()` renvoie -1). Fonctionne aussi
   en tapant du texte directement au clavier après avoir lancé le
   programme sans arguments (Ctrl-D ou la ligne `EOF` pour terminer).

Les trois modes ont été testés avec succès sur le scanner MiniCalc généré
depuis `spec/minicalc.max` :

```
$ ./minicalc_scanner /tmp/prog.txt              # mode fichier
$ printf "x = 1;\nEOF\n" | ./minicalc_scanner   # sentinelle EOF
$ printf "x = 1;"        | ./minicalc_scanner   # fin de flux normale
```

Les trois donnent la même tokenisation correcte, avec les IDs de token
exacts déclarés par `%token` (vérifié manuellement : `x`→`TOK_IDENT`(0),
`=`→`TOK_ASSIGN`(9), `3.14`→`TOK_FLOAT`(2), etc.)

### Fichier de tokens intermédiaire pour matyacc

`gen_tokens_file()` émet un format simple, une paire par ligne :

```
# Fichier de tokens genere par matlex (round 5) -- pour matyacc
TOK_IDENT 0
TOK_INT 1
TOK_FLOAT 2
...
```

matyacc (à venir) lira ce fichier pour connaître l'univers des terminaux
sans avoir à reparser la spec `.max` lexicale — même principe que le
`y.tab.h` généré par bison à partir des `%token` et consommé par le lexer
flex correspondant, mais dans l'autre sens (ici c'est le lexer qui informe
le parser).

### Aucun nouveau bug de compilateur

Comme au round 4, rien de nouveau côté compilateur. Deux erreurs trouvées
et corrigées, toutes deux dans **matlex lui-même** :
1. Chaîne `if/else if` mal générée (accolade fermante en trop à chaque
   règle, provenant d'une erreur de copier-coller dans le générateur).
2. Blocs d'action vides (`{ /* commentaire */ }`) dans la spec de
   démonstration, heurtant la limitation déjà connue du parseur — corrigé
   en remplissant chaque action vide d'une instruction triviale, et
   documenté comme contrainte du format `.max`.

---

## Round 6 — Définitions nommées, robustesse, API JFlex complète

### Prérequis compilateur : `#define`

Avant ce round, le compilateur MATL-ABC a été enrichi d'un préprocesseur
`#define` (macros objet, substitution textuelle consciente des chaînes/
commentaires — voir `matlabc_doc/23_preprocesseur_define.md`). matlex
l'exploite directement : les constantes de token (`%token`) sont désormais
émises comme `#define TOK_NOM id` dans le fichier généré, plutôt que comme
des fonctions à zéro argument (round 5) — les actions écrivent
`return TOK_IDENT;`, exactement comme en flex/bison classique.

### Définitions nommées `{NOM}`

```
LETTRE  = [a-zA-Z_]
CHIFFRE = [0-9]
ID      = {LETTRE}({LETTRE}|{CHIFFRE})*
ENTIER  = {CHIFFRE}+
```

Chaque définition est **expansée au moment de sa propre déclaration**
(`expand_refs()`) : `ID` peut réutiliser `LETTRE`/`CHIFFRE` déclarées plus
tôt, et stocke sa version **déjà entièrement expansée**
(`([a-zA-Z_])(([a-zA-Z_])|([0-9]))*`). Les règles référencent ensuite
`{ID}`, `{ENTIER}` normalement — y compris en les combinant :
`{ENTIER}\.{ENTIER}` pour un nombre flottant.

#### Piège rencontré et corrigé : `{NOM}` confondu avec le début d'une action

La détection round 5 de la frontière pattern/action cherchait naïvement le
**premier** `{` de la ligne — cassant tout pattern utilisant une référence
`{NOM}` (ex: `{ID} { return TOK_IDENT; }` était coupé après `{ID`, pas après
le vrai `{` de l'action). Corrigé en distinguant les deux cas : un `{`
immédiatement suivi d'un identifiant puis d'un `}` (sans rien d'autre) est
une référence de définition et fait partie du pattern ; tout autre `{` est
le début de l'action.

### Comptage d'accolades robuste (round 5 → round 6)

Le comptage d'accolades pour délimiter une action ignore désormais le
contenu des chaînes littérales `"..."` et caractères littéraux `'...'` — un
`}` à l'intérieur d'une chaîne (ex: `printf("erreur : }")`) ne referme plus
prématurément l'action.

### API JFlex complète

Générée comme méthodes/champs de la struct scanner :

| Membre | Type | Rôle |
|---|---|---|
| `self.yytext()` | méthode → `str` | texte reconnu |
| `self.yylength()` | méthode → `int` | longueur du lexème |
| `self.yychar()` | méthode → `int` | position absolue de début du token |
| `self.yyline` | champ `int` | numéro de ligne (toujours suivi) |
| `self.yycolumn` | champ `int` | numéro de colonne (toujours suivi) |
| `self.yystate()` | méthode → `int` | état lexical courant |
| `self.yybegin(s)` | méthode | change d'état lexical |
| `self.yypushback(n)` | méthode | remet `n` caractères dans le flux |
| `self.yyreset(src)` | méthode | réinitialise le scanner sur une nouvelle source |

**L'utilisateur écrit ses actions avec la syntaxe JFlex habituelle, SANS
`self.`** (`yytext()`, `yyline + 1`, ...) — `rewrite_action()` réécrit
automatiquement ces identifiants connus en `self.xxx` au moment de la
génération, conscient lui aussi des chaînes/caractères littéraux (ne
réécrit jamais leur contenu). C'est le compromis qui permet de respecter à
la fois la syntaxe JFlex familière à l'utilisateur et la règle MATL-ABC qui
exige un `self.` explicite pour accéder aux champs/méthodes de la struct
courante.

**Limitation assumée** : `yybegin`/`yystate` existent et fonctionnent
(l'état est bien mémorisé et changeable), mais ne **restreignent pas
encore** quelles règles peuvent matcher selon l'état courant (pas de
`<ETAT>pattern { action }` façon flex/JFlex) — toutes les règles sont
actives dans tous les états. Prévu pour un round ultérieur si le besoin de
« lexical states » complets se confirme (utile par exemple pour un scanner
de chaînes avec échappements, ou un mode commentaire imbriqué).

### Directives `%struct`, `%unicode`, `%line`, `%column`

- `%struct NOM` renomme la struct générée (`GenScanner` par défaut).
- `%line` / `%column` sont reconnues mais **sans effet supplémentaire** : le
  suivi ligne/colonne est de toute façon toujours actif dans le code
  généré (simplification assumée — évite une génération de code
  conditionnelle pour un gain marginal).
- `%unicode` est reconnue et **analysée mais sans effet** : le moteur reste
  octet-par-octet (`char` = 1 octet). Un vrai support Unicode/UTF-8 serait
  un chantier à part entière (classes de caractères sur des points de code
  multi-octets, DFA sur un alphabet bien plus large que 256 symboles),
  explicitement hors de portée ici.

### Nouveau bug de compilateur trouvé et corrigé

En testant `str yytext() { return &self.yytext_buf; }` (retour d'un champ
`char[256]` converti implicitement en `str`), l'émission LLVM échouait :
`strlen` était appelée avec un pointeur-vers-tableau (`[256 x i8]*`) au lieu
d'un pointeur simple (`i8*`) — même famille de bug que le round 1
(indexation d'un champ pointeur), mais ici pour la conversion implicite
`char* → str` dans `codegen_cast`. Corrigé en décroissant explicitement le
pointeur-vers-tableau (GEP `[0,0]`) avant tout usage comme `char*`, dans
`lib/backend/codegen.ml`. Régression vérifiée intacte (342 e2e + 122 unit)
avant et après le correctif.

### Vérification bout en bout

Spec `spec/minicalc2.max` (12 règles, 4 définitions imbriquées, 11 tokens)
→ générée → compilée séparément → exécutée sur `"x = 3.14 * (2 - 1) / y;"` :
tokens, `yytext()`, `yyline`, `yycolumn` tous corrects, y compris le suivi
de ligne sur une entrée multi-lignes testé séparément.

---

## Round 7 — struct `Symbol`, prologue en méthodes, retrait de `%token`

### Changement de philosophie

**`%token` a été retiré de matlex.** La déclaration des types de token
(leurs noms symboliques et la table qui les associe à des entiers) est le
rôle de **matyacc** (générateur de parser), pas de matlex (générateur de
lexer) — séparation des responsabilités classique, comme flex/bison ou
JFlex/CUP : le lexer ne connaît que des **entiers** de type de token,
jamais leurs noms symboliques (ceux-ci vivraient dans un module `sym`
généré par matyacc, à la manière de CUP).

À la place, matlex génère automatiquement une struct **`Symbol`** :

```matlabc
struct Symbol {
    int sym_type;   /* "type" est un mot reserve du langage */
    int line;
    int column;
    int ivalue;     /* valeur entiere optionnelle */
    str svalue;     /* valeur chaine optionnelle */
}
```

`yylex()` retourne désormais un `Symbol` (au lieu d'un simple `int`) —
`Symbol.sym_type == -1` signale la fin d'entrée.

### Le prologue devient du code À L'INTÉRIEUR du scanner

Différence majeure avec les rounds précédents : le bloc `%{ ... %}` n'est
plus imprimé **avant** la struct générée (comme du code libre top-level)
mais **dans son corps**, comme des méthodes supplémentaires — exactement
comme JFlex injecte le code utilisateur dans le corps de la classe
générée. L'utilisateur peut y définir des fonctions utilitaires
(`symbol(type)`, `symbolInt(type, v)`, `symbolStr(type, v)`, à la manière
de `symbol()`/`symbolObject()` en JFlex/CUP) qui accèdent normalement à
`yyline`/`yycolumn`/`yytext()` :

```matlabc
%{
Symbol symbol(int t) {
    Symbol s;
    s.sym_type = t;
    s.line = yyline;
    s.column = yycolumn;
    s.ivalue = 0;
    return s;
}
%}
```

`rewrite_action()` reécrit `yyline`/`yycolumn` en `self.yyline`/
`self.yycolumn` comme avant — et une nouvelle passe (`extract_prologue_fns`)
détecte automatiquement les fonctions définies dans le prologue (motif
`TYPE NOM(`) pour que tout **appel** à `symbol(...)` depuis une action de
règle soit lui aussi réécrit en `self.symbol(...)`, sans jamais toucher la
**déclaration** elle-même (repérée par sa position exacte, `decl_pos`,
distincte de chaque site d'appel).

### Deux bugs trouvés et corrigés pendant ce round

**1. Dans matlex** — `int type;` provoquait une erreur de syntaxe : `type`
est un mot réservé du langage MATL-ABC. Le champ a été renommé `sym_type`
dans la struct `Symbol` générée.

**2. Dans matlex, plus subtil** — les commentaires en français du prologue
(`l'utilisateur`, `d'elles`, `l'instant`...) sont pleins d'**apostrophes**,
que `rewrite_action()` confondait avec des ouvertures de littéral
caractère (`'x'`). Résultat : dès la première apostrophe rencontrée dans
un commentaire, tout le texte suivant était considéré comme "à l'intérieur
d'un caractère littéral" jusqu'à la prochaine apostrophe trouvée — parfois
bien plus loin dans le texte — désynchronisant complètement la réécriture
(`yyline` restait `yyline` au lieu de devenir `self.yyline`, provoquant une
erreur de compilation *"Variable non déclarée"* dans le code généré).
Corrigé en donnant à `rewrite_action()` la même conscience des commentaires
`/* ... */` et `// ...` que le préprocesseur `#define` du compilateur —
leur contenu est désormais copié verbatim, sans jamais entrer dans l'état
"chaîne/caractère littéral". Aucun bug de compilateur ici : uniquement
matlex, mais un piège réel et instructif (tout générateur de code French-
friendly doit se méfier des apostrophes dans les commentaires).

### Vérification bout en bout : `spec/minicalc3.max`

Spec complète (13 règles, mots-clés `if`/`else`/`while` prioritaires sur
`{ID}` par ordre de déclaration, valeurs `ivalue`/`svalue`, règle
catch-all `.` pour les caractères inconnus, **vrai `main()` en épilogue**
démontrant l'usage complet) → générée → compilée séparément → exécutée :

```
$ printf "x = 42;\nif y while foo @\nEOF\n" | ./minicalc3_scanner
=== MiniCalc3 : tokenisation avec Symbol ===

  IDENT   svalue='x'  ligne 0 colonne 1
  SYMBOLE type=24              ligne 0 colonne 3
  ENTIER  ivalue=42    ligne 0 colonne 6
  SYMBOLE type=25              ligne 0 colonne 7
  SYMBOLE type=10              ligne 1 colonne 2
  IDENT   svalue='y'  ligne 1 colonne 4
  SYMBOLE type=12              ligne 1 colonne 10
  IDENT   svalue='foo'  ligne 1 colonne 14
Caractere inconnu : '@' ligne 2 colonne 17

=== fin ===
```

`if`/`while` (type 10/12) correctement distingués de `y`/`foo` (IDENT),
`42` correctement converti en entier via `atoi(yytext().data())`,
caractère inconnu `@` correctement rapporté avec sa position — tout le
pipeline **spec → NFA → DFA → minimisation → code généré (Symbol +
prologue-en-méthodes + main() utilisateur) → compilation indépendante →
exécution** validé de bout en bout.

---

## Round 8 — Édition idiomatique du moteur

### Le constat

Les rounds 1 à 7 fonctionnent et sont testés, mais sont écrits dans un
style **essentiellement C** (traduit mot à mot en syntaxe MATL-ABC) :
`int` partout où `bool` s'imposait, `while` partout où `for i in
range(n)` aurait été plus court et plus sûr, des entiers magiques
(`0`/`1`/`2`/`3`) pour un type de transition plutôt qu'un `enum`, de
longues chaînes `if/else if` plutôt que `match`, des buffers `char*`
manuels plutôt que `str` natif. Le langage documenté dans
`docs/LANGUAGE.md` (génériques, `header`/`implements`, types optionnels,
`match` avec guard, f-strings, etc.) était à peine exploité à 10 % de
son potentiel.

### Ce qui a été fait dans ce round

Réécriture complète du **moteur** (`Frag`, `NFA`, `RegexParser`, `DFA`,
`DfaBuilder`, `DfaMinimizer` — le cœur algorithmique du projet, le plus
impactant à moderniser) en exploitant réellement les idiomes :

- **`bool` partout** où une valeur est logiquement booléenne (`changed`,
  `any_move`, `is_final`, `neg`, `done`, `processed[]`...) — plus un
  seul `int` utilisé comme drapeau 0/1.
- **`enum TransKind { TChar, TEps, TAny, TClass }`** à la place des
  entiers magiques 0/1/2/3 pour le type de transition, dispatché via
  **`match`** (`trans_accepts()`) plutôt qu'une chaîne `if/else if`.
  *Note technique* : un accès `Enum.Variant` produit un `int` (pas le
  type enum lui-même — cf. `docs/LANGUAGE.md`), donc le **stockage**
  (`int[4096] trans_kind`) reste `int`, mais les **valeurs** manipulées
  sont toujours les constantes nommées de l'enum, jamais des chiffres
  bruts — et le dispatch passe systématiquement par `match`.
- **`for i in range(n)`** pour toute boucle à borne connue à l'avance
  (remise à zéro d'un tableau, parcours 0..256, etc.) — les points
  fixes (`eps_closure`, minimisation) restent des `while` car leur
  condition d'arrêt est **dynamique**, pas une borne connue.
- **`str` native** (`.len()`, `.data()`) pour les patterns/entrées
  passés en paramètre (`test_pattern(str pattern, str input, ...)`) au
  lieu de `char*` partout — le `char*` reste utilisé en interne pour le
  parcours caractère par caractère (l'algorithme l'exige), mais
  l'**interface** publique des fonctions de test est en `str`.

### Piège trouvé en écrivant ce round

`TransKind k = TransKind.TChar;` échoue à la compilation
(*"type incompatible — attendu TransKind, obtenu int"*) : un accès
`Enum.Variant` est **toujours** typé `int`, jamais le type de l'enum
lui-même — cohérent avec la documentation (« les variants d'enum sont
des entiers »), mais un piège naturel en écrivant du code qui *semble*
vouloir déclarer une variable du type de l'enum. Pas un bug, juste un
comportement à connaître : un enum en MATL-ABC est une collection de
**constantes nommées**, pas un vrai type-somme distinct.

### Validation

**31/31 tests**, résultats **identiques** aux oracles des rounds 1
(Thompson, 15 tests) et 2 (classes/DFA/minimisation, 16 tests) —
compilé et exécuté **sans un seul bug** rencontré, malgré la surface
d'idiomes nouveaux couverte (`bool`, `enum`, `match` qualifié,
`for..in range`, `str.len()/.data()`, tableaux `bool[]` en champs de
struct et en locales). Un test de fumée séparé a d'abord validé chaque
idiome isolément (voir historique de session) avant la réécriture
complète, pour isoler rapidement d'éventuels problèmes — aucun trouvé.

### Portée honnête (mise à jour après le round 9)

Le round 9 a couvert la couche `.max`/JFlex (`MaxParser`, `rewrite_action`,
génération de scanner) dans le même esprit idiomatique — voir sa
section dédiée plus bas. **Reste dans son style d'origine (fonctionnel,
testé, mais moins idiomatique)** :

- **matyacc en entier** (les 4 rounds : LR(0), SLR(1), codegen, JCUP) —
  prochaine étape de cette migration.

**Recommandation pour la suite** : adopter systématiquement les
patterns de ce round 8 (`bool`, `enum`+`match`, `for-range`, `str`
native) pour tout nouveau code, et migrer matyacc dans le même esprit.

## Round 9 — Édition idiomatique de la couche `.max`/JFlex

### Ce qui a été fait

Bâti sur le moteur idiomatique du round 8 (`Frag`/`NFA`/`RegexParser`/
`DFA`/`DfaBuilder`/`DfaMinimizer`, repris tel quel), la couche `.max`/JFlex
(`MaxFile`, `MaxParser`, `rewrite_action`, `gen_scanner_v3` — l'équivalent
idiomatique des rounds 5 à 7) a été intégralement réécrite :

- **`bool`** pour tous les drapeaux de contrôle : `line_flag`/
  `column_flag`/`unicode_flag` (champs de `MaxFile`), les prédicats
  `is_inline_ws`/`is_ws_or_nl`/`is_ident_char`/`is_ident_start`/`at_end`
  (retournent désormais `bool`, plus `int`), et toutes les variables de
  contrôle de boucle (`closed`, `copying`, `done`, `scanning`,
  `scanning_pattern`, `in_str`, `in_chr`, `is_yy`, `already_qualified`,
  `same`, `has_epilogue_code`).
- **`for i in range(n)`** pour les boucles à borne statique connue à
  l'avance : génération de la table de transition DFA (`for s in
  range(dfa.state_count) { for b in range(256) { ... } }`), compilation
  des règles dans le NFA combiné (`for i in range(spec.rule_count)`),
  affichage des définitions/règles dans le diagnostic.

### Nouveauté nécessaire dans le moteur (round 8 → round 9)

Le moteur du round 8 ne gérait qu'un **seul motif** à la fois
(`compile()`). matlex a besoin de compiler **plusieurs règles dans un
seul NFA combiné** (le scanner doit reconnaître N motifs
simultanément). Ajout de `compile_rule(rule_id)` à `RegexParser` et
`accept_rule[]` à `NFA` (id de règle par état acceptant, `-1` sinon) —
`DfaBuilder.find_or_add()` calcule désormais la **règle gagnante**
(id minimal parmi les états acceptants du sous-ensemble, convention du
maximal munch déjà en vigueur depuis le round 3) plutôt qu'un simple
booléen accepté/rejeté.

### Piège rencontré en testant (pas un bug moteur)

Un test manuel a d'abord semblé **bloquer indéfiniment** (timeout) même
sur une entrée vide. Cause réelle : le fichier de test contenait **deux**
`main()` — celui généré par défaut par `gen_scanner_v3` (qui lit
`stdin` en boucle) et celui ajouté manuellement pour le test. Le
compilateur retient le premier rencontré ; le programme attendait donc
une entrée sur `stdin` qui n'arrivait jamais. Corrigé en retirant le
driver par défaut avant d'ajouter le `main()` de test — **aucun bug
dans matlex lui-même**, mais une leçon de méthodologie de test à noter
(toujours vérifier qu'il n'y a qu'un seul point d'entrée avant de
diagnostiquer un blocage comme un bug moteur).

### Validation

Même spec de démonstration (définitions imbriquées `LETTRE`/`CHIFFRE`/
`ID`) → mêmes résultats que le round 7 :

```
type=0  yytext='hello'  ligne=0 colonne=5
type=0  yytext='world'  ligne=0 colonne=11
type=1  yytext='42'  ligne=0 colonne=14
```

Et sur `spec/add_lexer.max` (prologue avec fonctions utilitaires
`symbol()`/`symbolInt()`, détection automatique via
`extract_prologue_fns`) — tokenisation de `"ADD 3 4"` :

```
type=0 yytext='ADD' ivalue=0
type=1 yytext='3' ivalue=3
type=1 yytext='4' ivalue=4
```

Identique à ce qu'attendait `matyacc` dans le test d'intégration
automatique du round 4 — la couche idiomatique reste **100 %
compatible** avec matyacc sans aucune modification côté matyacc.

**Mise à jour** : matyacc a depuis reçu le même traitement (round 5,
voir `projects/matyacc/docs/README.md`) — matlex et matyacc sont
désormais tous deux dans le style idiomatique.

## Reste à faire

- États lexicaux complets (`<ETAT>pattern { action }` façon flex/JFlex) :
  `yybegin`/`yystate` existent mais ne filtrent pas encore les règles
  actives.
- Écriture du code généré directement dans un fichier plutôt que sur
  `stdout` (actuellement imprimé puis redirigé manuellement via des
  marqueurs `=== GENERATED ... START/END ===` — `fopen`/`fputs` sont
  disponibles côté FFI mais nécessiteraient une conversion int→texte
  maison, `sprintf`/`fprintf` n'étant liés qu'en arité fixe, non variadique,
  dans ce compilateur).
- Vrai support Unicode/UTF-8 (`%unicode` actuellement reconnue sans effet).
- Section « définitions » de type `{n,m}` (quantificateurs bornés dans le
  moteur regex lui-même, hérité des rounds 1-2, toujours pas implémenté).
- Support des patterns entre guillemets `"if"` (littéraux échappés
  automatiquement, comme en flex/JFlex) — actuellement il faut écrire les
  mots-clés sans guillemets (`if` plutôt que `"if"`), ce qui fonctionne
  car aucune lettre n'est un métacaractère regex, mais un littéral
  contenant un métacaractère (`"+"`, `"*"`) demanderait l'échappement
  manuel (`\+`, `\*`) plutôt que la commodité des guillemets.
- `matyacc` (générateur de parser, projet frère, équivalent bison/menhir) :
  non commencé. Le format de fichier de tokens intermédiaire (round 5-6)
  a été retiré au round 7 — matyacc devra définir lui-même sa propre
  représentation des types de token, matlex ne fournissant plus que la
  struct `Symbol` générique comme interface.
