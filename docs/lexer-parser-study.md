# Étude comparative — lexer/parser maison vs réutilisation

Premier item du roadmap §1. Décision retenue : **lexer/pretty-printer maison**,
détaillée ci-dessous.

## Les options considérées

| | glslang (Khronos) | Tint (Dawn/WGSL) | DXC (Microsoft) | Maison |
|---|---|---|---|---|
| Langages couverts | GLSL uniquement | WGSL uniquement | HLSL uniquement | GLSL/HLSL/WGSL avec un seul cœur de lexer |
| But premier | Compiler vers SPIR-V | Compiler/transpiler WGSL | Compiler HLSL vers DXIL | Formater sans exécuter |
| Tolérance aux erreurs | Non — conçu pour rejeter du code invalide | Non | Non | Oui, nécessaire pour formater pendant la frappe |
| Préservation commentaires/mise en forme | Non — perdus dès le parsing (AST de compilateur, pas de CST) | Non | Non | Oui, objectif central |
| Poids de la dépendance | ~gros (C++, build CMake propre mais lourd, dépend de SPIRV-Tools) | Gros (fait partie de Dawn) | Très gros (LLVM/Clang) | Aucune dépendance externe |
| Un seul moteur pour les 3 langages | Non (3 bibliothèques distinctes, 3 AST différents) | Non | Non | Oui |

## Pourquoi pas de réutilisation

Les trois compilateurs de référence sont conçus pour **valider et compiler**
du code correct, pas pour **préserver la mise en forme** de code potentiellement
incomplet :

- Leurs AST sont des arbres de compilation (perte des commentaires, des
  espaces, parfois même de la casse des littéraux) : il faudrait les
  ré-instrumenter en profondeur pour en faire des CST (Concrete Syntax Tree)
  préservant tout, ce qui revient à réécrire une bonne partie du parser de
  toute façon.
- Aucun n'est tolérant aux erreurs : du code partiellement tapé (principe
  directeur du roadmap : jamais quitter l'éditeur) ferait planter ou
  rejeter le parsing.
- Il faudrait intégrer 3 bibliothèques différentes (glslang + Tint + DXC),
  chacune avec son propre système de build, pour couvrir GLSL+WGSL+HLSL —
  contraire au principe « un seul moteur réutilisable » (roadmap §5).
- Shadertoy (dialecte GLSL avec uniforms implicites) n'est pas un mode
  supporté nativement par glslang.

## Pourquoi un lexer/pretty-printer maison

- GLSL, HLSL et Shadertoy partagent une famille lexicale C-like quasi
  identique (accolades, opérateurs, commentaires `//` `/* */`,
  préprocesseur) : un seul tokenizer paramétrable suffit pour les trois.
  WGSL est syntaxiquement proche (mêmes commentaires, mêmes accolades,
  pas de préprocesseur, `@attributs`) et ne demande qu'un mode dédié sur
  le même tokenizer plutôt qu'un lexer entièrement séparé.
- Un formateur n'a pas besoin d'un AST complet côté sémantique (types,
  résolution de symboles) : la coloration et le ré-indentage peuvent se
  faire sur le flux de tokens (voir `Formatter.cpp`), ce qui réduit
  drastiquement la surface à construire pour le MVP tout en respectant
  strictement le principe « zéro modification sémantique » — puisque ces
  langages ne sont pas sensibles à l'espacement, ne toucher que les
  espaces/retours à la ligne entre tokens non réordonnés ne peut *pas*
  changer le comportement du shader.
- Garde le contrôle total sur la tolérance aux erreurs (aucune exception,
  jamais de crash, diagnostics best-effort) — nécessaire pour formater du
  code en cours de frappe, ce qu'aucun des trois compilateurs cités ne
  permet nativement.

## Limite assumée pour cette itération

Le formateur actuel (`Formatter.cpp`) est un **pretty-printer basé sur le
flux de tokens** (indentation/espacement pilotés par la profondeur
d'accolades/parenthèses), pas encore un AST complet avec règles de
regroupement structurel (alignement de déclarations, retour à la ligne
intelligent des appels longs, etc.). C'est suffisant et sûr pour le
formatage de base (§1) ; un AST plus riche reste une extension possible
pour les règles de style avancées du panneau de configuration (§4), sans
remettre en cause l'architecture actuelle (le lexer est déjà le socle
commun aux deux approches).

## Suite (§1) : un CST a été ajouté, en couche additive

Comme anticipé ci-dessus, un vrai parser/AST a été ajouté
(`lib/include/shaderfmt/Ast.hpp`, `lib/src/Parser.cpp`) **sans toucher au
lexer** : le lexer reste le socle commun, le parser consomme son flux de
tokens en sortie exactement comme le faisait l'ancien pretty-printer.

Décisions de scope pour cette itération :

- **CST *lossless*, pas un AST de compilateur.** Chaque token du lexer
  (y compris les commentaires) doit rester récupérable en parcourant
  l'arbre - c'est ce qui permet l'attachement de commentaires au bon
  nœud syntaxique (au lieu de l'heuristique de proximité précédente) et
  une tolérance aux erreurs qui ne perd/réordonne jamais un token. Les
  mots-clés à orthographe fixe et sans contenu variable (`if`/`for`/
  `while`/`do`/`switch`/`else`, les `{`/`}` de `Block`/`StructDecl`) ne
  sont pas stockés littéralement mais synthétisés depuis le type du
  nœud à l'émission - une duplication inutile puisqu'ils ne peuvent
  jamais varier autrement.
- **Grammaire volontairement peu profonde : pas d'arbre d'expressions.**
  Les expressions, listes de qualificatifs et arguments restent des
  runs de tokens opaques (`Chunk`). C'est le point central de cette
  étude : un formateur n'a besoin d'une structure que là où il doit
  *décider* quelque chose (où attache un commentaire, où recommencer
  après une erreur) - jamais à l'intérieur d'une expression, qu'il ne
  fait jamais que réespacer telle quelle.
- **Tolérance aux erreurs = `ErrorNode` + resynchronisation**, pas une
  grammaire complète de récupération à la Roslyn. Une construction non
  reconnue devient un `ErrorNode` (portion de tokens préservée telle
  quelle) et le parser reprend au prochain `;` de profondeur 0, à la
  prochaine `}` non consommée, ou à l'EOF. Testé directement (tests
  `ast_parser_never_throws_on_*`) et indirectement via les suites
  fuzz/troncature déjà existantes pour le lexer, désormais exécutées
  aussi de bout en bout via `format()`.
- **Limite connue et documentée, pas contournée en douce** : un
  `#ifdef`/`#else` qui s'ouvre au milieu d'une construction (plutôt
  qu'entre deux déclarations/instructions, le cas courant) fait échouer
  cette construction précise dans la grammaire et retombe en `ErrorNode`
  - toujours lossless, juste pas restructuré pour cette portion. Voir
  `README.md` § « Ce qui est simplifié » pour le détail et un exemple
  WGSL similaire (générique multi-arguments dans un champ de struct).
- **Bascule en 4 étapes vérifiées séparément**, pas un seul gros commit
  non testé : grammaire C-like seule (parser non branché dans
  `format()`, tests de forme/round-trip/tolérance) → grammaire WGSL même
  méthode → un `AstEmitter` construit en parallèle de l'ancien
  `Emitter`, prouvé strictement identique en sortie sur tout le corpus +
  500 entrées aléatoires + toutes les troncatures → bascule de
  `format()` sur ce chemin, retrait de l'ancien code mort. Chaque étape
  validée par un vrai build + `ctest` avant la suivante.
