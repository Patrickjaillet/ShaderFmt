# Architecture interne du moteur de formatage

Ce document est un guide de lecture pour un·e contributeur·rice qui
découvre `lib/` (`libshaderfmt`). Il ne duplique pas les commentaires déjà
présents dans le code - il indique où lire, dans quel ordre, et pourquoi
chaque étape existe. Pour "comment ajouter un langage/une règle de
style", voir [`CONTRIBUTING.md`](../CONTRIBUTING.md) ; pour l'historique
de la décision lexer-seul vs AST, voir
[`lexer-parser-study.md`](lexer-parser-study.md).

## Vue d'ensemble du pipeline

```
source (std::string)
    │  Lexer.cpp : lex(source, lang)
    ▼
std::vector<Token>              (Token.hpp)
    │  Parser.cpp : parse(tokens, lang)
    ▼
NodePtr (arbre CST)             (Ast.hpp)
    │  Formatter.cpp : emitNode() walk
    ▼
formatted (std::string)
```

`format()` (`Formatter.hpp`) est le seul point d'entrée public et
enchaîne ces trois étapes. Chacune est testable indépendamment (voir
`tests/test_main.cpp`) : le lexer et le parser ne lèvent jamais
d'exception, quelle que soit l'entrée.

## 1. Le lexer (`Lexer.{hpp,cpp}`)

Un tokenizer classique à la main (`class Scanner`, une seule classe
plate). Points notables :

- Un seul chemin de code sert GLSL/Shadertoy/HLSL/MSL/ShaderLab (même
  famille lexicale C-like) ; WGSL bifurque sur quelques réglages
  (préprocesseur désactivé, commentaires bloc imbriqués, token `@`).
- Chaque `Token` porte `onOwnLine`/`blankLinesBefore` : la mise en page
  d'origine (lignes vides, position sur sa propre ligne) est capturée dès
  le lexing, pas reconstruite plus tard par heuristique.
- Le préprocesseur (`#define`, `#include`, `#ifdef`...) est capturé comme
  un unique token opaque par ligne (`TokenType::Preprocessor`), jamais
  exécuté/interprété.
- Tolérant par construction : chaîne non terminée, commentaire bloc non
  fermé, etc. finissent dans `LexResult::errors`, sans jamais interrompre
  la tokenisation ni lever d'exception.

## 2. Le détecteur de langage (`LanguageDetector.{hpp,cpp}`)

`detectLanguage(source)` est une pure heuristique textuelle (pas de
lexing), utilisée uniquement quand l'utilisateur laisse le sélecteur sur
"Auto". Chaque dialecte a sa fonction `looksLikeXxx()` et l'ordre de
priorité dans `detectLanguage()` compte : un dialecte qui peut *contenir*
les signaux d'un autre (ShaderLab contenant du HLSL via `CGPROGRAM`) doit
être vérifié avant lui. Piège déjà rencontré et documenté dans le code :
éviter les sous-chaînes de mots anglais courants (`vertex`, `fragment`,
`kernel` isolés) comme signal, un commentaire GLSL les contient aussi
bien qu'un vrai fichier MSL - voir le test de non-régression
`detect_msl_does_not_false_positive_on_glsl_comment_mentioning_fragment`.

## 3. Le parser et l'arbre (`Parser.{hpp,cpp}`, `Ast.hpp`)

**Lire `Ast.hpp` en premier** : son commentaire d'en-tête et les
commentaires par valeur de `NodeKind` documentent précisément la forme de
l'arbre (layout de `children[]` pour chaque nœud, ce qui est structuré
vs. gardé comme run de tokens opaque). C'est la référence normative -
`Parser.cpp` ne fait qu'implémenter ce que `Ast.hpp` décrit.

Points de conception à retenir :

- **CST sans arbre d'expressions** : les expressions restent des runs de
  tokens opaques (`NodeKind::Chunk`), jamais parsées avec une précédence
  d'opérateurs. Le formateur ne fait jamais que déplacer des espaces/sauts
  de ligne entre des tokens non modifiés et non réordonnés - c'est
  suffisant pour garantir "aucune modification sémantique" sans grammaire
  d'expression complète (voir `docs/lexer-parser-study.md`).
- **Convention "mot-clé synthétisé"** : les nœuds structurels
  (`IfStmt`/`ForStmt`/`WhileStmt`/`DoWhileStmt`/`SwitchStmt`, `Block`,
  `StructDecl`, `ShaderDecl`, `ShaderLabBlock`) ne stockent pas leur
  propre mot-clé/accolade, puisque l'orthographe est 100% déterministe à
  partir de `NodeKind` - l'émetteur la resynthétise. Tout le reste stocke
  son contenu réel dans `tokens`.
- **Récupération tolérante** : toute construction qui ne matche aucune
  production tombe dans `ErrorNode` (`Parser.cpp`'s
  `recoverAsErrorNode()`), avec resynchronisation au prochain `;`/`}`/EOF.
  Aucun token n'est jamais perdu ni réordonné, même sur du code
  tronqué/invalide en cours de frappe.
- **Deux grammaires distinctes** : `parseCLikeProgram()`
  (GLSL/Shadertoy/HLSL/MSL, famille C-like partagée) et
  `parseWgslProgram()` (grammaire WGSL propre : `fn`/`let`/`var`/`const`,
  décorateurs `@attribute`, `->`, génériques `vec3<f32>`). ShaderLab a sa
  propre grammaire de bloc (`parseShaderLabProgram()` et associés) et
  invoque récursivement un `Parser` imbriqué en `Language::HLSL` sur les
  blocs `CGPROGRAM...ENDCG`/`HLSLPROGRAM...ENDHLSL` - un vrai
  sous-parsing, pas un blob opaque.
- **Commentaires attachés, pas positionnés par proximité** : chaque
  commentaire devient une `Trivia` (`leadingTrivia`/`trailingTrivia`) sur
  le nœud syntaxique qu'il précède réellement, décidé pendant le parsing.
- **Tolérance à la troncature** : `hasClosingBrace`/`hasBody` sur `Node`
  distinguent "ce bloc a réellement été fermé dans le texte source" de
  "le formateur devrait synthétiser une fermeture" - sans ces flags,
  formater un fichier tronqué en cours de frappe "compléterait" du code
  qui n'a jamais existé. Ces flags ont été ajoutés après un vrai bug trouvé
  par les tests de troncature (voir le fichier `CHANGELOG.md` /
  l'historique des tests fuzz/troncature).

## 4. L'émetteur (`Formatter.cpp`)

`emitNode()` parcourt l'arbre récursivement ; `class Emitter` porte l'état
d'indentation et les heuristiques d'espacement (déjà présentes avant
l'ajout de l'AST, réutilisées telles quelles - voir
`docs/lexer-parser-study.md` pour l'historique). Chaque `NodeKind` a un
`case` dans `emitNode()` qui sait comment le restituer : la plupart
suivent l'ordre par défaut "tokens puis enfants", quelques-uns ont un
ordre spécifique documenté en commentaire à leur `case`
(`CgProgramBlock` : marqueur ouvrant, programme imbriqué, marqueur
fermant - pas l'ordre par défaut).

## Où regarder pour une tâche donnée

- **Un shader se formate mal** (mauvais espacement/indentation) : le bug
  est presque toujours dans `Emitter` (`Formatter.cpp`), pas dans le
  parser - vérifiez d'abord que l'AST a la forme attendue (un test shape
  ciblé, voir les exemples `ast_msl_function_and_struct_with_attributes`
  / `ast_shaderlab_shape_and_embedded_hlsl_program` dans
  `tests/test_main.cpp`), puis seulement l'émission.
- **Un commentaire "saute" au mauvais endroit** : regardez comment le
  parser attache les `Trivia` pour la production concernée dans
  `Parser.cpp`.
- **Un fichier réel casse le parser/formateur** (pas de crash attendu,
  mais forme suspecte) : ajoutez-le au corpus (`tests/corpus/`), suivez
  le pattern de `tests/corpus/licensed/SOURCES.md` si la licence permet
  de le committer, et laissez les tests existants (round-trip,
  idempotence, non-régression de séquence de tokens, fuzz/troncature)
  s'exécuter dessus automatiquement via `corpusFiles()`.
- **Ajouter un dialecte** : voir `CONTRIBUTING.md`, qui liste l'ordre
  exact des fichiers à toucher, tiré de l'ajout réel de MSL et ShaderLab
  dans ce dépôt.
