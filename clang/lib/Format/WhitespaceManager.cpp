//===--- WhitespaceManager.cpp - Format C++ code --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file implements WhitespaceManager class.
///
//===----------------------------------------------------------------------===//
// clang-format off
#include "WhitespaceManager.h"
#include "llvm/ADT/STLExtras.h"

namespace clang {
namespace format {

bool WhitespaceManager::Change::IsBeforeInFile::operator()(
    const Change &C1, const Change &C2) const {
  return SourceMgr.isBeforeInTranslationUnit(
      C1.OriginalWhitespaceRange.getBegin(),
      C2.OriginalWhitespaceRange.getBegin());
}

WhitespaceManager::Change::Change(const FormatToken &Tok,
                                  bool CreateReplacement,
                                  SourceRange OriginalWhitespaceRange,
                                  int Spaces, unsigned StartOfTokenColumn,
                                  unsigned NewlinesBefore,
                                  StringRef PreviousLinePostfix,
                                  StringRef CurrentLinePrefix, bool IsAligned,
                                  bool ContinuesPPDirective, bool IsInsideToken)
    : Tok(&Tok), CreateReplacement(CreateReplacement),
      OriginalWhitespaceRange(OriginalWhitespaceRange),
      StartOfTokenColumn(StartOfTokenColumn), NewlinesBefore(NewlinesBefore),
      PreviousLinePostfix(PreviousLinePostfix),
      CurrentLinePrefix(CurrentLinePrefix), IsAligned(IsAligned),
      ContinuesPPDirective(ContinuesPPDirective), Spaces(Spaces),
      IsInsideToken(IsInsideToken), IsTrailingComment(false), TokenLength(0),
      PreviousEndOfTokenColumn(0), EscapedNewlineColumn(0),
      StartOfBlockComment(nullptr), IndentationOffset(0), ConditionalsLevel(0) {
}

void WhitespaceManager::replaceWhitespace(FormatToken &Tok, unsigned Newlines,
                                          unsigned Spaces,
                                          unsigned StartOfTokenColumn,
                                          bool IsAligned, bool InPPDirective) {
  if (Tok.Finalized)
    return;
  Tok.Decision = (Newlines > 0) ? FD_Break : FD_Continue;
  Changes.push_back(Change(Tok, /*CreateReplacement=*/true, Tok.WhitespaceRange,
                           Spaces, StartOfTokenColumn, Newlines, "", "",
                           IsAligned, InPPDirective && !Tok.IsFirst,
                           /*IsInsideToken=*/false));
}

void WhitespaceManager::addUntouchableToken(const FormatToken &Tok,
                                            bool InPPDirective) {
  if (Tok.Finalized)
    return;
  Changes.push_back(Change(Tok, /*CreateReplacement=*/false,
                           Tok.WhitespaceRange, /*Spaces=*/0,
                           Tok.OriginalColumn, Tok.NewlinesBefore, "", "",
                           /*IsAligned=*/false, InPPDirective && !Tok.IsFirst,
                           /*IsInsideToken=*/false));
}

llvm::Error
WhitespaceManager::addReplacement(const tooling::Replacement &Replacement) {
  return Replaces.add(Replacement);
}

void WhitespaceManager::replaceWhitespaceInToken(
    const FormatToken &Tok, unsigned Offset, unsigned ReplaceChars,
    StringRef PreviousPostfix, StringRef CurrentPrefix, bool InPPDirective,
    unsigned Newlines, int Spaces) {
  if (Tok.Finalized)
    return;
  SourceLocation Start = Tok.getStartOfNonWhitespace().getLocWithOffset(Offset);
  Changes.push_back(
      Change(Tok, /*CreateReplacement=*/true,
             SourceRange(Start, Start.getLocWithOffset(ReplaceChars)), Spaces,
             std::max(0, Spaces), Newlines, PreviousPostfix, CurrentPrefix,
             /*IsAligned=*/true, InPPDirective && !Tok.IsFirst,
             /*IsInsideToken=*/true));
}

// TALLY: Generate replacements with Tally customisations
const tooling::Replacements& WhitespaceManager::generateReplacements() {
    if (Changes.empty())
        return Replaces;

    llvm::sort(Changes, Change::IsBeforeInFile(SourceMgr));;
    calculateLineBreakInformation();

    columnarizeKeywords();                                // TALLY
    //columnarizePPKeywords();                              // TALLY
    //columnarizePPDefineKeyword();                         // TALLY. We do NOT use alignConsecutiveMacros().
    columnarizeDeclarationSpecifierTokens();              // TALLY
    columnarizeDatatypeTokens();                          // TALLY
    columnarizeNoDiscardOrNoReturnOrTemplate ();          // TALLY
    columnarizeIdentifierTokens();                        // TALLY
    columnarizeLParenTokensAndSplitArgs();                // TALLY

    alignConsecutiveAssignmentsOnScopedVarName();         // TALLY
    alignConsecutiveAssignmentsOnVarNameAcrossSections(); // TALLY
    alignConsecutiveAssignmentsOnVarNameWithinSection();  // TALLY
    alignConsecutiveVarBitFields();                       // TALLY. We do NOT use alignConsecutiveBitFields().
    alignConsecutiveAssignmentsOnEqualsAcrossSections();  // TALLY
    alignConsecutiveAssignmentsOnEqualsWithinSection();   // TALLY
    alignConsecutiveLBraceOfVarDeclOrDef();               // TALLY
    alignConsecutiveAssignementsOnUsing ();               // TALLY

    alignChainedConditionals();
    alignTrailingComments();
    alignEscapedNewlines();

    generateChanges();

    return Replaces;
}

/* TALLY: Commented out
const tooling::Replacements &WhitespaceManager::generateReplacements() {
  if (Changes.empty())
    return Replaces;

  llvm::sort(Changes, Change::IsBeforeInFile(SourceMgr));
  calculateLineBreakInformation();
  alignConsecutiveMacros();
  alignConsecutiveDeclarations();
  alignConsecutiveBitFields();
  alignConsecutiveAssignments();
  alignChainedConditionals();
  alignTrailingComments();
  alignEscapedNewlines();
  generateChanges();

  return Replaces;
}
*/

void WhitespaceManager::calculateLineBreakInformation() {
  Changes[0].PreviousEndOfTokenColumn = 0;
  Change *LastOutsideTokenChange = &Changes[0];

  for (unsigned i = 1, e = Changes.size(); i != e; ++i) {
    SourceLocation OriginalWhitespaceStart =
        Changes[i].OriginalWhitespaceRange.getBegin();
    SourceLocation PreviousOriginalWhitespaceEnd =
        Changes[i - 1].OriginalWhitespaceRange.getEnd();
    unsigned OriginalWhitespaceStartOffset =
        SourceMgr.getFileOffset(OriginalWhitespaceStart);
    unsigned PreviousOriginalWhitespaceEndOffset =
        SourceMgr.getFileOffset(PreviousOriginalWhitespaceEnd);
    assert(PreviousOriginalWhitespaceEndOffset <=
           OriginalWhitespaceStartOffset);
    const char *const PreviousOriginalWhitespaceEndData =
        SourceMgr.getCharacterData(PreviousOriginalWhitespaceEnd);
    StringRef Text(PreviousOriginalWhitespaceEndData,
                   SourceMgr.getCharacterData(OriginalWhitespaceStart) -
                       PreviousOriginalWhitespaceEndData);
    // Usually consecutive changes would occur in consecutive tokens. This is
    // not the case however when analyzing some preprocessor runs of the
    // annotated lines. For example, in this code:
    //
    // #if A // line 1
    // int i = 1;
    // #else B // line 2
    // int i = 2;
    // #endif // line 3
    //
    // one of the runs will produce the sequence of lines marked with line 1, 2
    // and 3. So the two consecutive whitespace changes just before '// line 2'
    // and before '#endif // line 3' span multiple lines and tokens:
    //
    // #else B{change X}[// line 2
    // int i = 2;
    // ]{change Y}#endif // line 3
    //
    // For this reason, if the text between consecutive changes spans multiple
    // newlines, the token length must be adjusted to the end of the original
    // line of the token.
    auto NewlinePos = Text.find_first_of('\n');
    if (NewlinePos == StringRef::npos) {
      Changes[i - 1].TokenLength = OriginalWhitespaceStartOffset -
                                   PreviousOriginalWhitespaceEndOffset +
                                   Changes[i].PreviousLinePostfix.size() +
                                   Changes[i - 1].CurrentLinePrefix.size();
    } else {
      Changes[i - 1].TokenLength =
          NewlinePos + Changes[i - 1].CurrentLinePrefix.size();
    }

    // If there are multiple changes in this token, sum up all the changes until
    // the end of the line.
    if (Changes[i - 1].IsInsideToken && Changes[i - 1].NewlinesBefore == 0)
      LastOutsideTokenChange->TokenLength +=
          Changes[i - 1].TokenLength + Changes[i - 1].Spaces;
    else
      LastOutsideTokenChange = &Changes[i - 1];

    Changes[i].PreviousEndOfTokenColumn =
        Changes[i - 1].StartOfTokenColumn + Changes[i - 1].TokenLength;

    Changes[i - 1].IsTrailingComment =
        (Changes[i].NewlinesBefore > 0 || Changes[i].Tok->is(tok::eof) ||
         (Changes[i].IsInsideToken && Changes[i].Tok->is(tok::comment))) &&
        Changes[i - 1].Tok->is(tok::comment) &&
        // FIXME: This is a dirty hack. The problem is that
        // BreakableLineCommentSection does comment reflow changes and here is
        // the aligning of trailing comments. Consider the case where we reflow
        // the second line up in this example:
        //
        // // line 1
        // // line 2
        //
        // That amounts to 2 changes by BreakableLineCommentSection:
        //  - the first, delimited by (), for the whitespace between the tokens,
        //  - and second, delimited by [], for the whitespace at the beginning
        //  of the second token:
        //
        // // line 1(
        // )[// ]line 2
        //
        // So in the end we have two changes like this:
        //
        // // line1()[ ]line 2
        //
        // Note that the OriginalWhitespaceStart of the second change is the
        // same as the PreviousOriginalWhitespaceEnd of the first change.
        // In this case, the below check ensures that the second change doesn't
        // get treated as a trailing comment change here, since this might
        // trigger additional whitespace to be wrongly inserted before "line 2"
        // by the comment aligner here.
        //
        // For a proper solution we need a mechanism to say to WhitespaceManager
        // that a particular change breaks the current sequence of trailing
        // comments.
        OriginalWhitespaceStart != PreviousOriginalWhitespaceEnd;
  }
  // FIXME: The last token is currently not always an eof token; in those
  // cases, setting TokenLength of the last token to 0 is wrong.
  Changes.back().TokenLength = 0;
  Changes.back().IsTrailingComment = Changes.back().Tok->is(tok::comment);

  WhitespaceManager::Change *LastBlockComment = nullptr;
  int PrevIdentifierTknStartOfTokenCntValue {};
  int PrevIdentifierTknSpacesValue {};
  int FirstCommnetWhiteSpaces {};

  for (auto &Change : Changes) {
    // Reset the IsTrailingComment flag for changes inside of trailing comments
    // so they don't get realigned later. Comment line breaks however still need
    // to be aligned.
    if (Change.Tok->Previous && Change.Tok->Previous->is(tok::l_brace)
            && Change.Tok->is(tok::identifier)
            && !(Change.Tok->IsClassScope || Change.Tok->IsStructScope
                || Change.Tok->IsEnumScope || Change.Tok->IsUnionScope)) {
        PrevIdentifierTknStartOfTokenCntValue = Change.StartOfTokenColumn;
        PrevIdentifierTknSpacesValue = Change.Spaces;
    }
    else if (PrevIdentifierTknStartOfTokenCntValue && Change.Spaces == 0
          && Change.Tok->is(tok::identifier) && !Change.Tok->Previous
          && Change.Tok->Next && Change.Tok->Next->is(tok::comma)
          && Change.Tok->LbraceCount
          && Change.Tok->NewlinesBefore && Change.Tok->LparenCount == 0) {
        Change.Spaces = PrevIdentifierTknSpacesValue;
        Change.StartOfTokenColumn = PrevIdentifierTknStartOfTokenCntValue;
    }

    if (Change.IsInsideToken && Change.NewlinesBefore == 0)
      Change.IsTrailingComment = false;
    Change.StartOfBlockComment = nullptr;
    Change.IndentationOffset = 0;
    if (Change.Tok->is(tok::comment)) {
      if (Change.Tok->is(TT_LineComment) || !Change.IsInsideToken)
          LastBlockComment = &Change;
      else {
        if ((Change.StartOfBlockComment == LastBlockComment))
          Change.IndentationOffset =
              Change.StartOfTokenColumn -
              Change.StartOfBlockComment->StartOfTokenColumn;
      }

      if (FirstCommnetWhiteSpaces == 0 && Change.Spaces && Change.Tok->LbraceCount) {
          FirstCommnetWhiteSpaces = Change.Spaces;
      }
      else if (Change.Spaces && Change.Tok->LbraceCount && PrevIdentifierTknSpacesValue) {
          if (Change.Tok->Previous && !Change.Tok->Previous->is(tok::comment))
              Change.Spaces -= PrevIdentifierTknSpacesValue;
      }
    } else {
      LastBlockComment = nullptr;
    }
  }
  PrevIdentifierTknStartOfTokenCntValue = {};
  PrevIdentifierTknSpacesValue = {};
  FirstCommnetWhiteSpaces = {};

  // Compute conditional nesting level
  // Level is increased for each conditional, unless this conditional continues
  // a chain of conditional, i.e. starts immediately after the colon of another
  // conditional.
  SmallVector<bool, 16> ScopeStack;
  int ConditionalsLevel = 0;
  for (auto &Change : Changes) {
    for (unsigned i = 0, e = Change.Tok->FakeLParens.size(); i != e; ++i) {
      bool isNestedConditional =
          Change.Tok->FakeLParens[e - 1 - i] == prec::Conditional &&
          !(i == 0 && Change.Tok->Previous &&
            Change.Tok->Previous->is(TT_ConditionalExpr) &&
            Change.Tok->Previous->is(tok::colon));
      if (isNestedConditional)
        ++ConditionalsLevel;
      ScopeStack.push_back(isNestedConditional);
    }

    Change.ConditionalsLevel = ConditionalsLevel;

    for (unsigned i = Change.Tok->FakeRParens; i > 0 && ScopeStack.size();
         --i) {
      if (ScopeStack.pop_back_val())
        --ConditionalsLevel;
    }
  }
}

// Align a single sequence of tokens, see AlignTokens below.
template <typename F>
static void
AlignTokenSequence(unsigned Start, unsigned End, unsigned Column, F &&Matches,
                   SmallVector<WhitespaceManager::Change, 16> &Changes) {
  bool FoundMatchOnLine = false;
  int Shift = 0;

  // ScopeStack keeps track of the current scope depth. It contains indices of
  // the first token on each scope.
  // We only run the "Matches" function on tokens from the outer-most scope.
  // However, we do need to pay special attention to one class of tokens
  // that are not in the outer-most scope, and that is function parameters
  // which are split across multiple lines, as illustrated by this example:
  //   double a(int x);
  //   int    b(int  y,
  //          double z);
  // In the above example, we need to take special care to ensure that
  // 'double z' is indented along with it's owning function 'b'.
  // Special handling is required for 'nested' ternary operators.
  SmallVector<unsigned, 16> ScopeStack;

  for (unsigned i = Start; i != End; ++i) {
    if (ScopeStack.size() != 0 &&
        Changes[i].indentAndNestingLevel() <
            Changes[ScopeStack.back()].indentAndNestingLevel())
      ScopeStack.pop_back();

    // Compare current token to previous non-comment token to ensure whether
    // it is in a deeper scope or not.
    unsigned PreviousNonComment = i - 1;
    while (PreviousNonComment > Start &&
           Changes[PreviousNonComment].Tok->is(tok::comment))
      PreviousNonComment--;
    if (i != Start && Changes[i].indentAndNestingLevel() >
                          Changes[PreviousNonComment].indentAndNestingLevel())
      ScopeStack.push_back(i);

    bool InsideNestedScope = ScopeStack.size() != 0;

    if (Changes[i].NewlinesBefore > 0 && !InsideNestedScope) {
      Shift = 0;
      FoundMatchOnLine = false;
    }

    // If this is the first matching token to be aligned, remember by how many
    // spaces it has to be shifted, so the rest of the changes on the line are
    // shifted by the same amount
    if (!FoundMatchOnLine && !InsideNestedScope && Matches(Changes[i])) {
      FoundMatchOnLine = true;
      Shift = Column - Changes[i].StartOfTokenColumn;
      Changes[i].Spaces += Shift;
    }

    // This is for function parameters that are split across multiple lines,
    // as mentioned in the ScopeStack comment.
    if (InsideNestedScope && Changes[i].NewlinesBefore > 0) {
      unsigned ScopeStart = ScopeStack.back();
      if (Changes[ScopeStart - 1].Tok->is(TT_FunctionDeclarationName) ||
          (ScopeStart > Start + 1 &&
           Changes[ScopeStart - 2].Tok->is(TT_FunctionDeclarationName)) ||
          Changes[i].Tok->is(TT_ConditionalExpr) ||
          (Changes[i].Tok->Previous &&
           Changes[i].Tok->Previous->is(TT_ConditionalExpr)))
        Changes[i].Spaces += Shift;
    }

    assert(Shift >= 0);
    Changes[i].StartOfTokenColumn += Shift;
    if (i + 1 != Changes.size())
      Changes[i + 1].PreviousEndOfTokenColumn += Shift;
  }
}

// Walk through a subset of the changes, starting at StartAt, and find
// sequences of matching tokens to align. To do so, keep track of the lines and
// whether or not a matching token was found on a line. If a matching token is
// found, extend the current sequence. If the current line cannot be part of a
// sequence, e.g. because there is an empty line before it or it contains only
// non-matching tokens, finalize the previous sequence.
// The value returned is the token on which we stopped, either because we
// exhausted all items inside Changes, or because we hit a scope level higher
// than our initial scope.
// This function is recursive. Each invocation processes only the scope level
// equal to the initial level, which is the level of Changes[StartAt].
// If we encounter a scope level greater than the initial level, then we call
// ourselves recursively, thereby avoiding the pollution of the current state
// with the alignment requirements of the nested sub-level. This recursive
// behavior is necessary for aligning function prototypes that have one or more
// arguments.
// If this function encounters a scope level less than the initial level,
// it returns the current position.
// There is a non-obvious subtlety in the recursive behavior: Even though we
// defer processing of nested levels to recursive invocations of this
// function, when it comes time to align a sequence of tokens, we run the
// alignment on the entire sequence, including the nested levels.
// When doing so, most of the nested tokens are skipped, because their
// alignment was already handled by the recursive invocations of this function.
// However, the special exception is that we do NOT skip function parameters
// that are split across multiple lines. See the test case in FormatTest.cpp
// that mentions "split function parameter alignment" for an example of this.
template <typename F>
static unsigned AlignTokens(const FormatStyle &Style, F &&Matches,
                            SmallVector<WhitespaceManager::Change, 16> &Changes,
                            unsigned StartAt) {
  unsigned MinColumn = 0;
  unsigned MaxColumn = UINT_MAX;

  // Line number of the start and the end of the current token sequence.
  unsigned StartOfSequence = 0;
  unsigned EndOfSequence = 0;

  // Measure the scope level (i.e. depth of (), [], {}) of the first token, and
  // abort when we hit any token in a higher scope than the starting one.
  auto IndentAndNestingLevel = StartAt < Changes.size()
                                   ? Changes[StartAt].indentAndNestingLevel()
                                   : std::tuple<unsigned, unsigned, unsigned>();

  // Keep track of the number of commas before the matching tokens, we will only
  // align a sequence of matching tokens if they are preceded by the same number
  // of commas.
  unsigned CommasBeforeLastMatch = 0;
  unsigned CommasBeforeMatch = 0;

  // Whether a matching token has been found on the current line.
  bool FoundMatchOnLine = false;

  // Aligns a sequence of matching tokens, on the MinColumn column.
  //
  // Sequences start from the first matching token to align, and end at the
  // first token of the first line that doesn't need to be aligned.
  //
  // We need to adjust the StartOfTokenColumn of each Change that is on a line
  // containing any matching token to be aligned and located after such token.
  auto AlignCurrentSequence = [&] {
    if (StartOfSequence > 0 && StartOfSequence < EndOfSequence)
      AlignTokenSequence(StartOfSequence, EndOfSequence, MinColumn, Matches,
                         Changes);
    MinColumn = 0;
    MaxColumn = UINT_MAX;
    StartOfSequence = 0;
    EndOfSequence = 0;
  };

  unsigned i = StartAt;
  for (unsigned e = Changes.size(); i != e; ++i) {
    if (Changes[i].indentAndNestingLevel() < IndentAndNestingLevel)
      break;

    if (Changes[i].NewlinesBefore != 0) {
      CommasBeforeMatch = 0;
      EndOfSequence = i;
      // If there is a blank line, there is a forced-align-break (eg,
      // preprocessor), or if the last line didn't contain any matching token,
      // the sequence ends here.
      if (Changes[i].NewlinesBefore > 1 ||
          Changes[i].Tok->MustBreakAlignBefore || !FoundMatchOnLine)
        AlignCurrentSequence();

      FoundMatchOnLine = false;
    }

    if (Changes[i].Tok->is(tok::comma)) {
      ++CommasBeforeMatch;
    } else if (Changes[i].indentAndNestingLevel() > IndentAndNestingLevel) {
      // Call AlignTokens recursively, skipping over this scope block.
      unsigned StoppedAt = AlignTokens(Style, Matches, Changes, i);
      i = StoppedAt - 1;
      continue;
    }

    if (!Matches(Changes[i]))
      continue;

    // If there is more than one matching token per line, or if the number of
    // preceding commas, do not match anymore, end the sequence.
    if (FoundMatchOnLine || CommasBeforeMatch != CommasBeforeLastMatch)
      AlignCurrentSequence();

    CommasBeforeLastMatch = CommasBeforeMatch;
    FoundMatchOnLine = true;

    if (StartOfSequence == 0)
      StartOfSequence = i;

    unsigned ChangeMinColumn = Changes[i].StartOfTokenColumn;
    int LineLengthAfter = Changes[i].TokenLength;
    for (unsigned j = i + 1; j != e && Changes[j].NewlinesBefore == 0; ++j) {
      LineLengthAfter += Changes[j].Spaces;
      // Changes are generally 1:1 with the tokens, but a change could also be
      // inside of a token, in which case it's counted more than once: once for
      // the whitespace surrounding the token (!IsInsideToken) and once for
      // each whitespace change within it (IsInsideToken).
      // Therefore, changes inside of a token should only count the space.
      if (!Changes[j].IsInsideToken)
        LineLengthAfter += Changes[j].TokenLength;
    }
    unsigned ChangeMaxColumn = Style.ColumnLimit - LineLengthAfter;

    // If we are restricted by the maximum column width, end the sequence.
    if (ChangeMinColumn > MaxColumn || ChangeMaxColumn < MinColumn ||
        CommasBeforeLastMatch != CommasBeforeMatch) {
      AlignCurrentSequence();
      StartOfSequence = i;
    }

    MinColumn = std::max(MinColumn, ChangeMinColumn);
    MaxColumn = std::min(MaxColumn, ChangeMaxColumn);
  }

  EndOfSequence = i;
  AlignCurrentSequence();
  return i;
}

// TALLY: Align a single sequence of tokens, see AlignTokens below.
template <typename F>
static void
AlignTokenSequence(unsigned Start, unsigned End, unsigned Column, F&& Matches,
    SmallVector<WhitespaceManager::Change, 16>& Changes,
    /* TALLY */ bool IgnoreScope, /* TALLY */ bool IgnoreCommas) {

    bool FoundMatchOnLine = false;
    int Shift = 0;

    // ScopeStack keeps track of the current scope depth. It contains indices of
    // the first token on each scope.
    // We only run the "Matches" function on tokens from the outer-most scope.
    // However, we do need to pay special attention to one class of tokens
    // that are not in the outer-most scope, and that is function parameters
    // which are split across multiple lines, as illustrated by this example:
    //   double a(int x);
    //   int    b(int  y,
    //          double z);
    // In the above example, we need to take special care to ensure that
    // 'double z' is indented along with it's owning function 'b'.
    SmallVector<unsigned, 16> ScopeStack;

    for (unsigned i = Start; i != End; ++i) {
        if (!IgnoreScope && ScopeStack.size() != 0 &&
            Changes[i].indentAndNestingLevel() <
            Changes[ScopeStack.back()].indentAndNestingLevel())
            ScopeStack.pop_back();

        // Compare current token to previous non-comment token to ensure whether
        // it is in a deeper scope or not.
        unsigned PreviousNonComment = i - 1;
        while (PreviousNonComment > Start &&
            Changes[PreviousNonComment].Tok->is(tok::comment))
            PreviousNonComment--;
        if (!IgnoreScope && i != Start && Changes[i].indentAndNestingLevel() >
            Changes[PreviousNonComment].indentAndNestingLevel())
            ScopeStack.push_back(i);

        bool InsideNestedScope = ScopeStack.size() != 0;

        if (Changes[i].NewlinesBefore > 0 &&
            (!InsideNestedScope || IgnoreScope)) {
            Shift = 0;
            FoundMatchOnLine = false;
        }

        if (!FoundMatchOnLine && (IgnoreScope || !InsideNestedScope) &&
            Matches(Changes[i])) {
            if (!(Changes[i].Tok->is(tok::identifier) && (i > 0)
                && Changes[i-1].Tok->isOneOf(tok::coloncolon, tok::l_square, tok::less))
                && Changes[i].Tok->LparenCount == 0) {
                FoundMatchOnLine = true;
                Shift = Column - Changes[i].StartOfTokenColumn;
                Changes[i].Spaces += Shift;
            }
        }


        // This is for function parameters that are split across multiple lines,
        // as mentioned in the ScopeStack comment.
        if (InsideNestedScope && Changes[i].NewlinesBefore > 0) {
            unsigned ScopeStart = ScopeStack.back();
            if (Changes[ScopeStart - 1].Tok->is(TT_FunctionDeclarationName) ||
                (ScopeStart > Start + 1 &&
                    Changes[ScopeStart - 2].Tok->is(TT_FunctionDeclarationName)))
                Changes[i].Spaces += Shift;
        }

        assert(Shift >= 0);
        Changes[i].StartOfTokenColumn += Shift;
        if (i + 1 != Changes.size())
            Changes[i + 1].PreviousEndOfTokenColumn += Shift;
    }
}

// TALLY: Walk through a subset of the changes, starting at StartAt, and find
// sequences of matching tokens to align. To do so, keep track of the lines and
// whether or not a matching token was found on a line. If a matching token is
// found, extend the current sequence. If the current line cannot be part of a
// sequence, e.g. because there is an empty line before it or it contains only
// non-matching tokens, finalize the previous sequence.
// The value returned is the token on which we stopped, either because we
// exhausted all items inside Changes, or because we hit a scope level higher
// than our initial scope.
// This function is recursive. Each invocation processes only the scope level
// equal to the initial level, which is the level of Changes[StartAt].
// If we encounter a scope level greater than the initial level, then we call
// ourselves recursively, thereby avoiding the pollution of the current state
// with the alignment requirements of the nested sub-level. This recursive
// behavior is necessary for aligning function prototypes that have one or more
// arguments.
// If this function encounters a scope level less than the initial level,
// it returns the current position.
// There is a non-obvious subtlety in the recursive behavior: Even though we
// defer processing of nested levels to recursive invocations of this
// function, when it comes time to align a sequence of tokens, we run the
// alignment on the entire sequence, including the nested levels.
// When doing so, most of the nested tokens are skipped, because their
// alignment was already handled by the recursive invocations of this function.
// However, the special exception is that we do NOT skip function parameters
// that are split across multiple lines. See the test case in FormatTest.cpp
// that mentions "split function parameter alignment" for an example of this.
template <typename F>
static unsigned AlignTokens(const FormatStyle& Style, F&& Matches,
    SmallVector<WhitespaceManager::Change, 16>& Changes,
    /* TALLY */ bool IgnoreScope,
    /* TALLY */ bool IgnoreCommas,
    unsigned StartAt,
    /* TALLY */ unsigned MaxNewlinesBeforeSectionBreak,
    /* TALLY */ bool NonMatchingLineBreaksSection,
    /* TALLY */ bool AllowBeyondColumnLimitForAlignment,
    /* TALLY */ unsigned MaxLeadingSpacesForAlignment,
    /* TALLY */ bool ForceAlignToFourSpaces 
) {
    // TALLY
    unsigned ColumnLimitInEffect = AllowBeyondColumnLimitForAlignment ? Style.ColumnLimitExtended : Style.ColumnLimit;

    unsigned MinColumn = 0;
    unsigned MaxColumn = UINT_MAX;

    // Line number of the start and the end of the current token sequence.
    unsigned StartOfSequence = 0;
    unsigned EndOfSequence = 0;

    // Measure the scope level (i.e. depth of (), [], {}) of the first token, and
    // abort when we hit any token in a higher scope than the starting one.
    auto IndentAndNestingLevel = StartAt < Changes.size()
        ? Changes[StartAt].indentAndNestingLevel()
        : std::tuple<unsigned, unsigned, unsigned>(0, 0, 0);

    // Keep track of the number of commas before the matching tokens, we will only
    // align a sequence of matching tokens if they are preceded by the same number
    // of commas.
    unsigned CommasBeforeLastMatch = 0;
    unsigned CommasBeforeMatch = 0;

    // Whether a matching token has been found on the current line.
    bool FoundMatchOnLine = false;

    // Aligns a sequence of matching tokens, on the MinColumn column.
    //
    // Sequences start from the first matching token to align, and end at the
    // first token of the first line that doesn't need to be aligned.
    //
    // We need to adjust the StartOfTokenColumn of each Change that is on a line
    // containing any matching token to be aligned and located after such token.
    auto AlignCurrentSequence = [&] {
        if (StartOfSequence > 0 && StartOfSequence < EndOfSequence)
            AlignTokenSequence(StartOfSequence, EndOfSequence, MinColumn, Matches,
                Changes, IgnoreScope, IgnoreCommas);
        MinColumn = 0;
        MaxColumn = UINT_MAX;
        StartOfSequence = 0;
        EndOfSequence = 0;
    };

    unsigned i = StartAt;
    for (unsigned e = Changes.size(); i != e; ++i) {
        if (Changes[i].indentAndNestingLevel() < IndentAndNestingLevel)
            break;

        if (Changes[i].NewlinesBefore != 0) {
            CommasBeforeMatch = 0;
            EndOfSequence = i;
            // If there is a blank line, or if the last line didn't contain any
            // matching token, the sequence ends here.
            if (Changes[i].NewlinesBefore > MaxNewlinesBeforeSectionBreak || (NonMatchingLineBreaksSection && !FoundMatchOnLine))
                AlignCurrentSequence();

            FoundMatchOnLine = false;
        }

        if (Changes[i].Tok->is(tok::comma)) {
            ++CommasBeforeMatch;
        }
        else if (!IgnoreScope && (Changes[i].indentAndNestingLevel() > IndentAndNestingLevel)) {
            // Call AlignTokens recursively, skipping over this scope block.
            unsigned StoppedAt =
                AlignTokens(Style, Matches, Changes, IgnoreScope, IgnoreCommas, i,
                    MaxNewlinesBeforeSectionBreak, NonMatchingLineBreaksSection, 
                    AllowBeyondColumnLimitForAlignment, MaxLeadingSpacesForAlignment, ForceAlignToFourSpaces);
            i = StoppedAt - 1;
            continue;
        }

        if (!Matches(Changes[i]))
            continue;

        // If there is more than one matching token per line, or if the number of
        // preceding commas, do not match anymore, end the sequence.
        if (FoundMatchOnLine ||
            (!IgnoreCommas && (CommasBeforeMatch != CommasBeforeLastMatch)))
            AlignCurrentSequence();

        CommasBeforeLastMatch = CommasBeforeMatch;
        FoundMatchOnLine = true;

        if (StartOfSequence == 0)
            StartOfSequence = i;

        unsigned ChangeMinColumn = Changes[i].StartOfTokenColumn;
        int LineLengthAfter = -Changes[i].Spaces;
        for (unsigned j = i; j != e && Changes[j].NewlinesBefore == 0; ++j)
            LineLengthAfter += Changes[j].Spaces + Changes[j].TokenLength;
        unsigned ChangeMaxColumn = ColumnLimitInEffect - LineLengthAfter;

        int leadingSpacesReqd = ChangeMinColumn - MinColumn;
        if (leadingSpacesReqd < 0) {
            leadingSpacesReqd *= -1;
        }
        // If we are restricted by the maximum leading spaces limit or maximum column width, end the sequence.
        if (
            (unsigned)leadingSpacesReqd > MaxLeadingSpacesForAlignment ||
            ChangeMinColumn > MaxColumn ||
            ChangeMaxColumn < MinColumn ||
            (!IgnoreCommas && (CommasBeforeLastMatch != CommasBeforeMatch))
            ) {
            AlignCurrentSequence();
            StartOfSequence = i;
        }

        MinColumn = std::max(MinColumn, ChangeMinColumn);
        MaxColumn = std::min(MaxColumn, ChangeMaxColumn);

        // TALLY: Force align to four spaces
        if (ForceAlignToFourSpaces) {
            if (MinColumn % 4 != 0) {
                unsigned int pad = 4 - (MinColumn % 4);
                MinColumn += pad;
                MaxColumn += pad;
            }
        }
    }

    EndOfSequence = i;
    AlignCurrentSequence();
    return i;
}

// TALLY: Align assignments on scoped variable name (WITHIN SECTION)
void WhitespaceManager::alignConsecutiveAssignmentsOnScopedVarName() {
    if (!Style.AlignConsecutiveAssignments)
        return;

    AlignTokens(Style,
        [&](const Change& C) {

            bool retval = 
                (C.Tok->isScopedVarNameInDecl() &&
                C.Tok->HasSemiColonInLine);

            return retval;
        },
        Changes, /*IgnoreScope=*/false, /*IgnoreCommas=*/false, /*StartAt=*/0,
            /*MaxNewlinesBeforeSectionBreak=*/2, /*NonMatchingLineBreaksSection=*/true,
            /*AllowBeyondColumnLimitForAlignment=*/true, /*MaxLeadingSpacesForAlignment=*/UINT_MAX, 
            /*ForceAlignToFourSpaces*/false);
}

// TALLY : Align the consecutive constexprs
void WhitespaceManager::alignConsecutiveLBraceOfVarDeclOrDef() {
    if (!Style.AlignConsecutiveAssignments)
        return;

    AlignTokens(Style,
        [&](const Change& C) {
            return C.Tok->isLBraceOfConstexprOrVarDelcOrDef() && 
                   C.Tok->HasSemiColonInLine; // Ensure is terminated by a semi colon.
        },
        Changes, /*IgnoreScope=*/false, /*IgnoreCommas=*/false, /*StartAt=*/0,
            /*MaxNewlinesBeforeSectionBreak=*/2, /*NonMatchingLineBreaksSection=*/true,
            /*AllowBeyondColumnLimitForAlignment=*/true, /*MaxLeadingSpacesForAlignment=*/UINT_MAX, 
            /*ForceAlignToFourSpaces*/true);
}

void WhitespaceManager::alignConsecutiveAssignementsOnUsing () {
    if (!Style.AlignConsecutiveAssignments)
        return;

    AlignTokens(Style,
        [&](const Change& C) {
            bool retval =  (C.Tok->MyLine && C.Tok->MyLine->First &&
                            C.Tok->MyLine->First->is(tok::kw_using) &&
                            C.Tok->HasSemiColonInLine && 
                            C.Tok->Previous && C.Tok->Previous->Previous && C.Tok->Previous->Previous == C.Tok->MyLine->First &&
                            C.Tok->is(tok::equal));

            return retval;
        },
        Changes, /*IgnoreScope=*/false, /*IgnoreCommas=*/false, /*StartAt=*/0,
            /*MaxNewlinesBeforeSectionBreak=*/2, /*NonMatchingLineBreaksSection=*/true,
            /*AllowBeyondColumnLimitForAlignment=*/true, /*MaxLeadingSpacesForAlignment=*/UINT_MAX, 
            /*ForceAlignToFourSpaces*/true);
}

// TALLY: Align assignments on variable name (ACROSS SECTIONS)
// Mutually exclusive with alignConsecutiveAssignmentsOnVarNameWithinSection()
void WhitespaceManager::alignConsecutiveAssignmentsOnVarNameAcrossSections() {
    if (!Style.AlignConsecutiveAssignments)
        return;

    AlignTokens(Style,
        [&](const Change& C) {

            return
                C.Tok->isVarNameInDecl() &&
                C.Tok->HasSemiColonInLine &&
                C.Tok->LbraceCount > 0 &&
                C.Tok->IsClassScope;
        },
        Changes, /*IgnoreScope=*/false, /*IgnoreCommas=*/false, /*StartAt=*/0,
            /*MaxNewlinesBeforeSectionBreak=*/2, /*NonMatchingLineBreaksSection=*/false,
            /*AllowBeyondColumnLimitForAlignment=*/true, /*MaxLeadingSpacesForAlignment=*/UINT_MAX,
            /*ForceAlignToFourSpaces*/false);
}

// TALLY: Align assignments on variable name (WITHIN SECTION)
// Mutually exclusive with alignConsecutiveAssignmentsOnVarNameAcrossSections()
void WhitespaceManager::alignConsecutiveAssignmentsOnVarNameWithinSection() {
    if (!Style.AlignConsecutiveAssignments)
        return;

    AlignTokens(Style,
        [&](const Change& C) {

            bool retval = 
                C.Tok->isVarNameInDecl() &&
                C.Tok->HasSemiColonInLine &&
                !C.Tok->IsClassScope;

            return retval;
        },
        Changes, /*IgnoreScope=*/false, /*IgnoreCommas=*/false, /*StartAt=*/0,
            /*MaxNewlinesBeforeSectionBreak=*/2, /*NonMatchingLineBreaksSection=*/false,
            /*AllowBeyondColumnLimitForAlignment=*/true, /*MaxLeadingSpacesForAlignment=*/UINT_MAX,
            /*ForceAlignToFourSpaces*/true);
}

// TALLY: Align on bit field colon in a variable declaration (ACROSS SECTIONS)
void WhitespaceManager::alignConsecutiveVarBitFields() {

    AlignTokens(Style,
        [&](const Change& C) {

            // Do not align on ':' that is first on a line.
            if (C.NewlinesBefore > 0)
                return false;

            // Do not align on ':' that is last on a line.
            if (&C != &Changes.back() && (&C + 1)->NewlinesBefore > 0)
                return false;

            return C.Tok->is(TT_BitFieldColon);
            /*
            return
                C.Tok->is(tok::colon) &&
                C.Tok->HasSemiColonInLine &&
                C.Tok->getPreviousNonComment() != nullptr &&
                C.Tok->getPreviousNonComment()->isVarNameInDecl();
            */
        },
        Changes, /*IgnoreScope=*/false, /*IgnoreCommas=*/false, /*StartAt=*/0,
            /*MaxNewlinesBeforeSectionBreak=*/2, /*NonMatchingLineBreaksSection=*/false,
            /*AllowBeyondColumnLimitForAlignment=*/true, /*MaxLeadingSpacesForAlignment=*/UINT_MAX,
            /*ForceAlignToFourSpaces*/false);
}

/// TALLY: Align consecutive assignments over all \c Changes (ACROSS SECTIONS)
// Mutually exclusive with alignConsecutiveAssignmentsWithinSection()
void WhitespaceManager::alignConsecutiveAssignmentsOnEqualsAcrossSections() {
    if (!Style.AlignConsecutiveAssignments)
        return;

    AlignTokens(Style,
        [&](const Change& C) {
            // Do not align on equal signs that are first on a line.
            if (C.NewlinesBefore > 0)
                return false;

            // Do not align on equal signs that are last on a line.
            if (&C != &Changes.back() && (&C + 1)->NewlinesBefore > 0)
                return false;

            return
                C.Tok->is(tok::equal) &&
                C.Tok->HasSemiColonInLine &&
                C.Tok->getPreviousNonComment() != nullptr &&
                C.Tok->getPreviousNonComment()->isVarNameInDecl();
        },
        Changes, /*IgnoreScope=*/false, /*IgnoreCommas=*/false, /*StartAt=*/0,
            /*MaxNewlinesBeforeSectionBreak=*/2, /*NonMatchingLineBreaksSection=*/false,
            /*AllowBeyondColumnLimitForAlignment=*/true, /*MaxLeadingSpacesForAlignment=*/16,
            /*ForceAlignToFourSpaces*/false);
}

/// TALLY: Align consecutive assignments over all \c Changes (WITHIN SECTION)
// Mutually exclusive with alignConsecutiveAssignmentsDeclAcrossSections()
void WhitespaceManager::alignConsecutiveAssignmentsOnEqualsWithinSection() {
    if (!Style.AlignConsecutiveAssignments)
        return;

    AlignTokens(Style,
        [&](const Change& C) {
            // Do not align on equal signs that are first on a line.
            if (C.NewlinesBefore > 0)
                return false;

            // Do not align on equal signs that are last on a line.
            if (&C != &Changes.back() && (&C + 1)->NewlinesBefore > 0)
                return false;

            return
                C.Tok->is(tok::equal) &&
                C.Tok->HasSemiColonInLine &&
                C.Tok->isPrevBeforeInterimsVarWithoutDatatype();
        },
        Changes, /*IgnoreScope=*/false, /*IgnoreCommas=*/false, /*StartAt=*/0,
            /*MaxNewlinesBeforeSectionBreak=*/1, /*NonMatchingLineBreaksSection=*/true,
            /*AllowBeyondColumnLimitForAlignment=*/true, /*MaxLeadingSpacesForAlignment=*/12,
            /*ForceAlignToFourSpaces*/false);
}

/// TALLY: Columnarize specific tokens over all \c Changes.
void WhitespaceManager::columnarizePPKeywords() {
    // First loop
    for (int i = 0; i < Changes.size(); ++i) {
        const FormatToken* MyTok = Changes[i].Tok;

        if (MyTok->isPPKeywordAndPrevHash()) {
            size_t tokSize = ((StringRef)MyTok->TokenText).size() + 1;
            MaxPPKeywordLen = MaxPPKeywordLen < tokSize ? tokSize : MaxPPKeywordLen;
        }
    }

    unsigned toPad = MaxPPKeywordLen + 1;
    unsigned pad = 1;

    while (toPad % Style.TabWidth != 0) {
        toPad++;
        pad++;
    }

    // Second loop
    for (int i = 0; i < Changes.size(); ++i) {

        const FormatToken* MyTok = Changes[i].Tok;
        const FormatToken* PrevTok = MyTok->getPreviousNonComment();

        if (PrevTok && PrevTok->isPPKeywordAndPrevHash()) {

            size_t tokSize = ((StringRef)PrevTok->TokenText).size() + 1;
            size_t lenDiff = MaxPPKeywordLen - tokSize;

            Changes[i].Spaces = pad + lenDiff;
        }
    }
}

/// TALLY: Columnarize specific tokens over all \c Changes.
void WhitespaceManager::columnarizePPDefineKeyword() {
    // First loop
    for (int i = 0; i < Changes.size(); ++i) {
        const FormatToken* MyTok = Changes[i].Tok;

        if (MyTok->isPPDefineKeywordAndPrevHash()) {
            const FormatToken* NextTok = MyTok->getNextNonComment();
            if (NextTok) {
                size_t tokSize = ((StringRef)NextTok->TokenText).size();
                MaxPPDefineLHSLen = MaxPPDefineLHSLen < tokSize ? tokSize : MaxPPDefineLHSLen;
            }
        }
    }

    unsigned toPad = MaxPPDefineLHSLen + 1;
    unsigned pad = 1;

    while (toPad % Style.TabWidth != 0) {
        toPad++;
        pad++;
    }

    // Second loop
    for (int i = 0; i < Changes.size(); ++i) {

        const FormatToken* PrevTok = Changes[i].Tok->getPreviousNonComment();

        if (PrevTok) {
            const FormatToken* PrevPrevTok = PrevTok->getPreviousNonComment();

            if (PrevPrevTok && PrevPrevTok->isPPDefineKeywordAndPrevHash()) {
                // PrevTok is LHS
                size_t tokSize = ((StringRef)PrevTok->TokenText).size();
                size_t lenDiff = MaxPPDefineLHSLen - tokSize;
                // Spaces before RHS
                Changes[i].Spaces = pad + lenDiff;
            }
        }
    }
}

// TALLY: Columnarize keywords tokens over all \c Changes.
void WhitespaceManager::columnarizeKeywords()
{
    int spacebefswitch {};

    if (!Style.AlignConsecutiveDeclarations)
        return;

    for (int i = 0; i < Changes.size(); ++i) {

        const FormatToken* MyTok = Changes[i].Tok;

        if (MyTok->IsInFunctionDefinitionScope) {

            if (MyTok->is(tok::kw_switch)) {

                spacebefswitch = Changes[i].Spaces;
                continue;
            }

            if (MyTok->is(tok::kw_case)) {

                Changes[i].Spaces = spacebefswitch + 4;
                Changes[i].StartOfTokenColumn = spacebefswitch + 4;
                continue;
            }
        }
    }
}

/// TALLY: Columnarize specific tokens over all \c Changes.
// TODO: Check 'template' use-cases and adapt
// TODO: Works only if declaration specifiers and datatypes do not have inline comments between the tokens.
// TODO: Assumes tab size is 4. Need to fix to accept variable tab sizes.
void WhitespaceManager::columnarizeDeclarationSpecifierTokens() {
    unsigned dummy;

    if (!Style.AlignConsecutiveDeclarations)
        return;

    for (int i = 0; i < Changes.size(); ++i) {

        const FormatToken* MyTok = Changes[i].Tok;

        // 'const' is also applicable to params in addition to being a decl specifier, so filter out on LparenCount
        if ((!(MyTok->IsClassScope || MyTok->IsStructScope)) || MyTok->LbraceCount == 0 || MyTok->LparenCount > 0)
            continue;

        const FormatToken* PrevTok = MyTok->getPreviousNonComment();
        if (PrevTok && PrevTok->isAfterNoDiscardOrNoReturnOrTemplate(Changes[i].NewlinesBefore)) {
            PrevTok = nullptr;
        }

        // 'const' is also applicable after parens, so filter out such tokens
        if (MyTok->is(tok::kw_const) && PrevTok && PrevTok->is(tok::r_paren))
            continue;

        // Filter out the template token since they lie in a seperate line itself.
        if (MyTok->isAfterNoDiscardOrNoReturnOrTemplate(dummy))
            continue;

        AnnotatedLine* MyLine = MyTok->MyLine;

        // As spaces before 'static' or 'virtual' has been set to zero, if static or virtual 
        // is not the first specifier in the list, then it will concatenate with the preceding
        // specifier.
        if ((MyTok->isDeclSpecStaticOrVirtual() && PrevTok == nullptr) || (MyTok->isDeclSpecInlineOrExtern() && MyTok->getNextNonComment()->isDeclSpecStaticOrVirtual())) {
            Changes[i].StartOfTokenColumn = 0;
            Changes[i].Spaces = 0;
            MyLine->LastSpecifierPadding = MyTok->is(tok::kw_static) ? 2 : 1; // len(static)=6, len(virtual)=7
            MyLine->LastSpecifierTabs += 2;
            MaxSpecifierTabs = MaxSpecifierTabs < MyLine->LastSpecifierTabs ? MyLine->LastSpecifierTabs : MaxSpecifierTabs;
        }
        else if (MyTok->isDeclarationSpecifier()) {

            if (PrevTok && PrevTok->isDeclSpecStaticOrVirtual()) {
                Changes[i].Spaces = MyLine->LastSpecifierPadding;
            }
            else {
                if (MyLine->LastSpecifierTabs == 0) {
                    MyLine->LastSpecifierTabs = 2;
                    if (MyTok->is(tok::kw_friend))
                        Changes[i].Spaces = MyTok->NewlinesBefore != 0 ? MyLine->LastSpecifierTabs * Style.TabWidth : 1;
                }
                else {
                    Changes[i].Spaces = MyLine->LastSpecifierPadding;
                }
            }

            Changes[i].StartOfTokenColumn = MyLine->LastSpecifierTabs * Style.TabWidth;

            // len=5
            if (MyTok->is(tok::kw_const)) {
                MyLine->LastSpecifierPadding = 3;
                if (!PrevTok && (MyLine->MightBeFunctionDecl || MyTok->IsStructScope)) {
                    Changes[i].Spaces += MyLine->LastSpecifierPadding + 1;
                    Changes[i].StartOfTokenColumn += 4;
                }
                MyLine->LastSpecifierTabs += 2;
            }
            // len=6
            else if (MyTok->isOneOf(tok::kw_inline, tok::kw_friend, tok::kw_extern)) {
                   MyLine->LastSpecifierPadding = 2;
                   MyLine->LastSpecifierTabs += 2;
            }
            // len=7
            else if (MyTok->is(tok::kw_mutable)) {
                MyLine->LastSpecifierPadding = 1;
                MyLine->LastSpecifierTabs += 2;
            }
            // len=8
            else if (MyTok->isOneOf(tok::kw_volatile, tok::kw_explicit, tok::kw_register)) {
                MyLine->LastSpecifierPadding = 4;
                MyLine->LastSpecifierTabs += 3;
            }
            // len=9
            else if (MyTok->is(tok::kw_constexpr)) {
                MyLine->LastSpecifierPadding = 3;
                if (!PrevTok && (MyLine->MightBeFunctionDecl || MyTok->IsStructScope)) {
                    Changes[i].Spaces += MyLine->LastSpecifierPadding + 1;
                    Changes[i].StartOfTokenColumn += 4;
                }
                MyLine->LastSpecifierTabs += 3;
            }
            // len=12
            else if (MyTok->is(tok::kw_thread_local)) {
                MyLine->LastSpecifierPadding = 4;
                MyLine->LastSpecifierTabs += 4;
            }
            // variable length
            else if (MyTok->is(tok::kw_alignas)) {
                const FormatToken* NextTok = MyTok->getNextNonComment();
                size_t interimSize = 0;
                while (NextTok && !NextTok->is(tok::r_paren))
                {
                    interimSize += NextTok->SpacesRequiredBefore;
                    interimSize += ((StringRef)NextTok->TokenText).size();
                    NextTok = NextTok->getNextNonComment();
                }
                if (NextTok && NextTok->is(tok::r_paren)) {
                    interimSize += NextTok->SpacesRequiredBefore;
                    interimSize++;
                    int toPad = 7 + interimSize;
                    while (toPad % Style.TabWidth != 0)
                        toPad++;
                    MyLine->LastSpecifierPadding = toPad - (7 + interimSize);
                    MyLine->LastSpecifierTabs += (toPad / 4);
                }
            }
            MaxSpecifierTabs = MaxSpecifierTabs < MyLine->LastSpecifierTabs ? MyLine->LastSpecifierTabs : MaxSpecifierTabs;
        }
    }
}

/// TALLY: Columnarize specific tokens over all \c Changes.
// TODO: Works only if declaration specifiers and datatypes do not have inline comments between the tokens.

void WhitespaceManager::columnarizeDatatypeTokens() {
  if (!Style.AlignConsecutiveDeclarations)
    return;

  if (MaxSpecifierTabs < 4)
    MaxSpecifierTabs = 4;

  int bracecount = 0;

  for (int i = 0; i < Changes.size(); ++i) {

    const FormatToken *MyTok = Changes[i].Tok;

    if (!(MyTok->IsClassScope || MyTok->IsStructScope) || MyTok->LbraceCount == 0 || MyTok->LparenCount > 0) 
      continue;

    if (MyTok->is(tok::less) && MyTok->Previous->is(tok::kw_template)) {

      ++bracecount;
      continue;
    }

    if (bracecount) {

      if (MyTok->is(tok::greater))
        --bracecount;
      continue;
    }

    if (MyTok->IsDatatype) {

        AnnotatedLine *MyLine = MyTok->MyLine;

        bool functionNameAfterInterims = MyTok->isFunctionNameAfterInterims();
        bool memVarNameAfterInterims = MyTok->isMemberVariableNameAfterInterims();

        bool ismaybeunused = (MyTok->Previous && MyTok->Previous->Previous && MyTok->Previous->Previous->isMaybeUnused() && MyLine->First == MyTok->Previous->Previous);

      if (functionNameAfterInterims || memVarNameAfterInterims || ismaybeunused) {
          int j = ismaybeunused ? i - 2 : i;

          size_t tokSize = ((StringRef)MyTok->TokenText).size();

          FormatToken *NextTok = MyTok->getNextNonCommentNonConst();

          size_t interimSize = 0;

        while (NextTok && NextTok->IsInterimBeforeName) {

          interimSize += NextTok->SpacesRequiredBefore;
          interimSize += ((StringRef)NextTok->TokenText).size();
          NextTok = NextTok->getNextNonCommentNonConst();
        }

        if (ismaybeunused) {
          NextTok = NextTok->getNextNonCommentNonConst ();
          if (NextTok) {
            NextTok = NextTok->getNextNonCommentNonConst();
            NextTok->PrevTokenSizeForColumnarization = tokSize;
            NextTok->IsDatatype = true;
          }
        }

        tokSize = ismaybeunused ? 4 + tokSize : interimSize + tokSize;

        if (NextTok) {

          if ((functionNameAfterInterims && NextTok->isFunctionAndNextLeftParen()) ||
              (memVarNameAfterInterims && NextTok->isMemberVarNameInDecl())) {

            NextTok->PrevTokenSizeForColumnarization = tokSize;
          }
        }

        MaxDatatypeLen = MaxDatatypeLen < tokSize ? tokSize : MaxDatatypeLen;

        if (MyLine->LastSpecifierTabs == 0 || MyLine->First->isMaybeUnused()) {

          Changes[j].Spaces = MaxSpecifierTabs * Style.TabWidth;
          MyLine->LastSpecifierTabs = MaxSpecifierTabs;
        }
        else if (MyLine->LastSpecifierTabs < MaxSpecifierTabs) {

          Changes[j].Spaces = ((MaxSpecifierTabs - MyLine->LastSpecifierTabs) * Style.TabWidth) + MyLine->LastSpecifierPadding;
          MyLine->LastSpecifierTabs = MaxSpecifierTabs;
        }
        else if (MyLine->LastSpecifierTabs == MaxSpecifierTabs) {

          Changes[j].Spaces = MyLine->LastSpecifierPadding;
        }

        Changes[j].StartOfTokenColumn = MaxSpecifierTabs * Style.TabWidth;
      }
    }
  }
}

/// TALLY : For checking if the function decl in class body is a template/nodiscard/noreturn type.
void WhitespaceManager::columnarizeNoDiscardOrNoReturnOrTemplate() {
  int spacecount;
  int bracecount;

  if (!Style.AlignConsecutiveDeclarations)
    return;

  if (MaxSpecifierTabs < 4)
    MaxSpecifierTabs = 4;

  for (int i = 0; i < Changes.size(); ++i) {

    const FormatToken *MyTok = Changes[i].Tok;

    // Space before [[maybe_unused]]
    if (MyTok->is(tok::l_square) && MyTok->Next && MyTok->Next->is(tok::l_square)
        && MyTok->LparenCount
        && (!MyTok->Previous || (MyTok->Previous && MyTok->Previous->is(tok::comment)))) {
      Changes[i].Spaces = 1;
    } // arrangement like LocatorType & pLocation
    else if (MyTok->is(tok::identifier) && MyTok->Previous
        && MyTok->Previous->isOneOf(tok::amp, tok::ampamp, tok::star)
        && MyTok->Previous->Previous && MyTok->Previous->Previous->is(tok::identifier)
        && MyTok->LparenCount) {
      Changes[i].Spaces = 1;
    }

    if ((!(MyTok->IsClassScope || MyTok->IsStructScope)) || MyTok->LbraceCount == 0 || MyTok->LparenCount > 0)
      continue;

    if (MyTok->isNoDiscardOrNoReturnOrTemplate()) {

      if (MyTok->is(tok::l_square)) {

        Changes[i].StartOfTokenColumn = MaxSpecifierTabs * Style.TabWidth;
        Changes[i].Spaces = MaxSpecifierTabs * Style.TabWidth;

          const FormatToken *next = MyTok->getNextNonComment();

        if (next && next->is(tok::l_square)) {

          ++i;
          Changes[i].StartOfTokenColumn = MaxSpecifierTabs * Style.TabWidth;
          Changes[i].Spaces = 0;

          next = next->getNextNonComment();
          if (next && (next->TokenText.startswith("nodiscard") || next->TokenText.startswith("noreturn"))) {

            ++i;
            Changes[i].StartOfTokenColumn += MaxSpecifierTabs * Style.TabWidth;
            Changes[i].Spaces = 0;

            next = next->getNextNonComment();
            if (next && next->is(tok::r_square)) {

              ++i;
              Changes[i].StartOfTokenColumn += MaxSpecifierTabs * Style.TabWidth;
              Changes[i].Spaces = 0;

              next = next->getNextNonComment();
              if (next && next->is(tok::r_square)) {

                ++i;
                Changes[i].StartOfTokenColumn += MaxSpecifierTabs * Style.TabWidth;
                Changes[i].Spaces = 0;
              }
            }
          }
        }
      }
    
      FormatToken * aftertemplate = (FormatToken * )MyTok->walkTemplateBlockInClassDecl();
      bool isfrienddecl = aftertemplate ? aftertemplate->is(tok::kw_friend) : false;

      if (MyTok->is(tok::kw_template) && 
          MyTok->MyLine && 
          (MyTok->MyLine->MightBeFunctionDecl || isfrienddecl)&& 
          MyTok->MyLine->endsWith(tok::semi) && 
          MyTok->LbraceCount > 0 && 
          MyTok->LparenCount == 0 && 
          MyTok->LArrowCount == 0) {

        spacecount = 0;
        bracecount = 0;
        
        Changes[i].StartOfTokenColumn = isfrienddecl ? 8 : MaxSpecifierTabs * Style.TabWidth;
        Changes[i].Spaces = isfrienddecl ? 8 : MaxSpecifierTabs * Style.TabWidth;;

        ++i;
        const FormatToken *curr = MyTok->Next;

        if (curr->is(tok::less)) {
            spacecount = 0;
            ++bracecount;
        }

        while (bracecount) {

            curr = curr->getNextNonComment();
            ++i;

            if (!curr)
                break;

            if (curr->is(tok::less)) {
                spacecount = 0;
                ++bracecount;
            }

            if (curr->is(tok::greater)) {
                spacecount = 0;
                --bracecount;
            }

            if (curr->is(tok::comma) || curr->is(tok::ellipsis)) {
                spacecount = 0;
            }

            if (curr->isOneOf (tok::kw_template, tok::kw_typename, tok::kw_class) ||
                curr->isDatatype()) {
              if (curr->Previous && curr->Previous->isOneOf(tok::ellipsis, tok::less, tok::coloncolon))
                spacecount = 0;
              else 
                spacecount = 1;
            }

            if (curr->is(tok::identifier)) {
                if (curr->Previous && curr->Previous->isOneOf(tok::less, tok::coloncolon)) // tok::ellipsis,
                    spacecount = 0;
                else
                    spacecount = 1;
            }

            Changes[i].StartOfTokenColumn += MaxSpecifierTabs * Style.TabWidth;
            Changes[i].Spaces = spacecount;
        } while (bracecount != 0);
      }
    }
  }
}

/// TALLY: Columnarize specific tokens over all \c Changes.
void WhitespaceManager::columnarizeIdentifierTokens() {
    if (!Style.AlignConsecutiveDeclarations)
        return;

    unsigned toPad = MaxDatatypeLen + 1;
    unsigned pad = 1;

    while (toPad % Style.TabWidth != 0) {
        toPad++;
        pad++;
    }

    for (int i = 0; i < Changes.size(); ++i) {

        const FormatToken* MyTok = Changes[i].Tok;

        if ((!(MyTok->IsClassScope || MyTok->IsStructScope)) && (MyTok->LbraceCount == 0 || MyTok->LparenCount > 0))
            continue;

        // Dont align bitfield specifiers.
        if (MyTok->Previous && MyTok->Previous->is(TT_BitFieldColon))
            continue;

        FormatToken* NextTok = MyTok->getNextNonCommentNonConst();

        if (MyTok->isMemberVarNameInDecl()) {

            size_t lenDiff = MaxDatatypeLen - MyTok->PrevTokenSizeForColumnarization;
            
            if (MyTok->Previous && MyTok->Previous->is(tok::l_brace) && MyTok->Next && !MyTok->Next->is(tok::comment)) {
                Changes[i].Spaces = 0;
            }
            else {
                Changes[i].Spaces = pad + lenDiff;
            }

            int j = i + 1;

            while (j < Changes.size() && Changes[j].NewlinesBefore == 0) {
                Changes [j].StartOfTokenColumn += lenDiff;
                ++j;
            }
        }
        else if (MyTok->isFunctionNameAndPrevIsPointerOrRefOrDatatype() && !MyTok->IsInFunctionDefinitionScope) {
            size_t lenDiff = MaxDatatypeLen - MyTok->PrevTokenSizeForColumnarization;
            Changes[i].Spaces = pad + lenDiff;
            int j = i + 1;

            size_t tokSize = ((StringRef)MyTok->TokenText).size();
            MaxMemberNameLen = MaxMemberNameLen < tokSize ? tokSize : MaxMemberNameLen;

            while (j < Changes.size() && Changes[j].NewlinesBefore == 0) {
                Changes [j].StartOfTokenColumn += lenDiff;
                ++j;
            }

            if (NextTok)
                NextTok->PrevTokenSizeForColumnarization = tokSize;
        }
        else if (MyTok->isConstructor()) {
            Changes[i].Spaces = MaxSpecifierTabs * Style.TabWidth + MaxDatatypeLen + pad;
            size_t tokSize = ((StringRef)MyTok->TokenText).size();
            MaxMemberNameLen = MaxMemberNameLen < tokSize ? tokSize : MaxMemberNameLen;

            if (NextTok)
                NextTok->PrevTokenSizeForColumnarization = tokSize;
        }
        else if (MyTok->isDestructor()) {
            Changes[i].Spaces = MaxSpecifierTabs * Style.TabWidth + MaxDatatypeLen + pad;
            if (Changes[i].Spaces > 1)
                Changes[i].Spaces -= 1;

            if (NextTok) {
                // Size of 'next'
                size_t tokSize = ((StringRef)NextTok->TokenText).size();
                MaxMemberNameLen = MaxMemberNameLen < tokSize ? tokSize : MaxMemberNameLen;

                FormatToken* NextNextTok = NextTok->getNextNonCommentNonConst();

                if (NextNextTok)
                    NextNextTok->PrevTokenSizeForColumnarization = tokSize;
            }
        }
        else if (MyTok->is(tok::l_brace) && MyTok->Previous
                && MyTok->Previous->is(tok::identifier)
                && MyTok->Previous->Previous
                && MyTok->Previous->Previous->is(tok::star)) {
            // populate MaxGlobalVarNameLen
            if (MaxGlobalVarNameLen < MyTok->Previous->ColumnWidth)
                MaxGlobalVarNameLen = MyTok->Previous->ColumnWidth;
        }
    }
}

/// TALLY: Columnarize specific tokens over all \c Changes.
void WhitespaceManager::columnarizeLParenTokensAndSplitArgs() {
    bool insideargs = false;
    int newlineargssize = 0;

    if (!Style.AlignConsecutiveDeclarations)
        return;

    unsigned toPad = MaxMemberNameLen + 1;
    unsigned pad = 1;

    while (toPad % Style.TabWidth != 0) {
        toPad++;
        pad++;
    }

    for (int i = 0; i < Changes.size(); ++i) {

        const FormatToken* MyTok = Changes[i].Tok;

        if ((!(MyTok->IsClassScope || MyTok->IsStructScope)) && MyTok->LbraceCount == 0)
            continue;

        FormatToken* PrevTok = MyTok->getPreviousNonComment();

        if (MyTok->is(tok::l_paren) && !MyTok->IsInFunctionDefinitionScope && PrevTok && PrevTok->isFunctionOrCtorOrPrevIsDtor()) {
            size_t lenDiff = MaxMemberNameLen - MyTok->PrevTokenSizeForColumnarization;
            Changes[i].Spaces = pad + lenDiff;
            newlineargssize = toPad + MaxSpecifierTabs * Style.TabWidth + MaxDatatypeLen + pad + 2;
            insideargs = true;
        }
        else if (MyTok->is(tok::l_brace) && PrevTok && PrevTok->is(tok::identifier)
                && MyTok->Previous->Previous && MyTok->Previous->Previous->is(tok::star)) {
            size_t lenDiff = MaxGlobalVarNameLen - PrevTok->ColumnWidth;
            Changes[i].Spaces = 1 + lenDiff;
            insideargs = false;
        }

        if (insideargs && MyTok->is(tok::r_paren))
            insideargs = false;

        if (insideargs)
            if (MyTok->NewlinesBefore > 0)
                Changes[i].Spaces = newlineargssize;
    }
}

// Aligns a sequence of matching tokens, on the MinColumn column.
//
// Sequences start from the first matching token to align, and end at the
// first token of the first line that doesn't need to be aligned.
//
// We need to adjust the StartOfTokenColumn of each Change that is on a line
// containing any matching token to be aligned and located after such token.
static void AlignMacroSequence(
    unsigned &StartOfSequence, unsigned &EndOfSequence, unsigned &MinColumn,
    unsigned &MaxColumn, bool &FoundMatchOnLine,
    std::function<bool(const WhitespaceManager::Change &C)> AlignMacrosMatches,
    SmallVector<WhitespaceManager::Change, 16> &Changes) {
  if (StartOfSequence > 0 && StartOfSequence < EndOfSequence) {

    FoundMatchOnLine = false;
    int Shift = 0;

    for (unsigned I = StartOfSequence; I != EndOfSequence; ++I) {
      if (Changes[I].NewlinesBefore > 0) {
        Shift = 0;
        FoundMatchOnLine = false;
      }

      // If this is the first matching token to be aligned, remember by how many
      // spaces it has to be shifted, so the rest of the changes on the line are
      // shifted by the same amount
      if (!FoundMatchOnLine && AlignMacrosMatches(Changes[I])) {
        FoundMatchOnLine = true;
        Shift = MinColumn - Changes[I].StartOfTokenColumn;
        Changes[I].Spaces += Shift;
      }

      assert(Shift >= 0);
      Changes[I].StartOfTokenColumn += Shift;
      if (I + 1 != Changes.size())
        Changes[I + 1].PreviousEndOfTokenColumn += Shift;
    }
  }

  MinColumn = 0;
  MaxColumn = UINT_MAX;
  StartOfSequence = 0;
  EndOfSequence = 0;
}

void WhitespaceManager::alignConsecutiveMacros() {
  if (!Style.AlignConsecutiveMacros)
    return;

  auto AlignMacrosMatches = [](const Change &C) {
    const FormatToken *Current = C.Tok;
    unsigned SpacesRequiredBefore = 1;

    if (Current->SpacesRequiredBefore == 0 || !Current->Previous)
      return false;

    Current = Current->Previous;

    // If token is a ")", skip over the parameter list, to the
    // token that precedes the "("
    if (Current->is(tok::r_paren) && Current->MatchingParen) {
      Current = Current->MatchingParen->Previous;
      SpacesRequiredBefore = 0;
    }

    if (!Current || !Current->is(tok::identifier))
      return false;

    if (!Current->Previous || !Current->Previous->is(tok::pp_define))
      return false;

    // For a macro function, 0 spaces are required between the
    // identifier and the lparen that opens the parameter list.
    // For a simple macro, 1 space is required between the
    // identifier and the first token of the defined value.
    return Current->Next->SpacesRequiredBefore == SpacesRequiredBefore;
  };

  unsigned MinColumn = 0;
  unsigned MaxColumn = UINT_MAX;

  // Start and end of the token sequence we're processing.
  unsigned StartOfSequence = 0;
  unsigned EndOfSequence = 0;

  // Whether a matching token has been found on the current line.
  bool FoundMatchOnLine = false;

  unsigned I = 0;
  for (unsigned E = Changes.size(); I != E; ++I) {
    if (Changes[I].NewlinesBefore != 0) {
      EndOfSequence = I;
      // If there is a blank line, or if the last line didn't contain any
      // matching token, the sequence ends here.
      if (Changes[I].NewlinesBefore > 1 || !FoundMatchOnLine)
        AlignMacroSequence(StartOfSequence, EndOfSequence, MinColumn, MaxColumn,
                           FoundMatchOnLine, AlignMacrosMatches, Changes);

      FoundMatchOnLine = false;
    }

    if (!AlignMacrosMatches(Changes[I]))
      continue;

    FoundMatchOnLine = true;

    if (StartOfSequence == 0)
      StartOfSequence = I;

    unsigned ChangeMinColumn = Changes[I].StartOfTokenColumn;
    int LineLengthAfter = -Changes[I].Spaces;
    for (unsigned j = I; j != E && Changes[j].NewlinesBefore == 0; ++j)
      LineLengthAfter += Changes[j].Spaces + Changes[j].TokenLength;
    unsigned ChangeMaxColumn = Style.ColumnLimit - LineLengthAfter;

    MinColumn = std::max(MinColumn, ChangeMinColumn);
    MaxColumn = std::min(MaxColumn, ChangeMaxColumn);
  }

  EndOfSequence = I;
  AlignMacroSequence(StartOfSequence, EndOfSequence, MinColumn, MaxColumn,
                     FoundMatchOnLine, AlignMacrosMatches, Changes);
}

void WhitespaceManager::alignConsecutiveAssignments() {
  if (!Style.AlignConsecutiveAssignments)
    return;

  AlignTokens(
      Style,
      [&](const Change &C) {
        // Do not align on equal signs that are first on a line.
        if (C.NewlinesBefore > 0)
          return false;

        // Do not align on equal signs that are last on a line.
        if (&C != &Changes.back() && (&C + 1)->NewlinesBefore > 0)
          return false;

        return C.Tok->is(tok::equal);
      },
      Changes, /*StartAt=*/0);
}

void WhitespaceManager::alignConsecutiveBitFields() {
  if (!Style.AlignConsecutiveBitFields)
    return;

  AlignTokens(
      Style,
      [&](Change const &C) {
        // Do not align on ':' that is first on a line.
        if (C.NewlinesBefore > 0)
          return false;

        // Do not align on ':' that is last on a line.
        if (&C != &Changes.back() && (&C + 1)->NewlinesBefore > 0)
          return false;

        return C.Tok->is(TT_BitFieldColon);
      },
      Changes, /*StartAt=*/0);
}

void WhitespaceManager::alignConsecutiveDeclarations() {
  if (!Style.AlignConsecutiveDeclarations)
    return;

  // FIXME: Currently we don't handle properly the PointerAlignment: Right
  // The * and & are not aligned and are left dangling. Something has to be done
  // about it, but it raises the question of alignment of code like:
  //   const char* const* v1;
  //   float const* v2;
  //   SomeVeryLongType const& v3;
  AlignTokens(
      Style,
      [](Change const &C) {
        // tok::kw_operator is necessary for aligning operator overload
        // definitions.
        if (C.Tok->isOneOf(TT_FunctionDeclarationName, tok::kw_operator))
          return true;
        if (C.Tok->isNot(TT_StartOfName))
          return false;
        // Check if there is a subsequent name that starts the same declaration.
        for (FormatToken *Next = C.Tok->Next; Next; Next = Next->Next) {
          if (Next->is(tok::comment))
            continue;
          if (!Next->Tok.getIdentifierInfo())
            break;
          if (Next->isOneOf(TT_StartOfName, TT_FunctionDeclarationName,
                            tok::kw_operator))
            return false;
        }
        return true;
      },
      Changes, /*StartAt=*/0);
}

void WhitespaceManager::alignChainedConditionals() {
  if (Style.BreakBeforeTernaryOperators) {
    AlignTokens(
        Style,
        [](Change const &C) {
          // Align question operators and last colon
          return C.Tok->is(TT_ConditionalExpr) &&
                 ((C.Tok->is(tok::question) && !C.NewlinesBefore) ||
                  (C.Tok->is(tok::colon) && C.Tok->Next &&
                   (C.Tok->Next->FakeLParens.size() == 0 ||
                    C.Tok->Next->FakeLParens.back() != prec::Conditional)));
        },
        Changes, /*StartAt=*/0);
  } else {
    static auto AlignWrappedOperand = [](Change const &C) {
      auto Previous = C.Tok->getPreviousNonComment(); // Previous;
      return C.NewlinesBefore && Previous && Previous->is(TT_ConditionalExpr) &&
             (Previous->is(tok::question) ||
              (Previous->is(tok::colon) &&
               (C.Tok->FakeLParens.size() == 0 ||
                C.Tok->FakeLParens.back() != prec::Conditional)));
    };
    // Ensure we keep alignment of wrapped operands with non-wrapped operands
    // Since we actually align the operators, the wrapped operands need the
    // extra offset to be properly aligned.
    for (Change &C : Changes) {
      if (AlignWrappedOperand(C))
        C.StartOfTokenColumn -= 2;
    }
    AlignTokens(
        Style,
        [this](Change const &C) {
          // Align question operators if next operand is not wrapped, as
          // well as wrapped operands after question operator or last
          // colon in conditional sequence
          return (C.Tok->is(TT_ConditionalExpr) && C.Tok->is(tok::question) &&
                  &C != &Changes.back() && (&C + 1)->NewlinesBefore == 0 &&
                  !(&C + 1)->IsTrailingComment) ||
                 AlignWrappedOperand(C);
        },
        Changes, /*StartAt=*/0);
  }
}

void WhitespaceManager::alignTrailingComments() {
  unsigned MinColumn = 0;
  unsigned MaxColumn = UINT_MAX;
  unsigned StartOfSequence = 0;
  bool BreakBeforeNext = false;
  unsigned Newlines = 0;
  for (unsigned i = 0, e = Changes.size(); i != e; ++i) {
    if (Changes[i].StartOfBlockComment)
      continue;
    Newlines += Changes[i].NewlinesBefore;
    if (Changes[i].Tok->MustBreakAlignBefore)
      BreakBeforeNext = true;
    if (!Changes[i].IsTrailingComment)
      continue;

    unsigned ChangeMinColumn = Changes[i].StartOfTokenColumn;
    unsigned ChangeMaxColumn;

    if (Style.ColumnLimit == 0)
      ChangeMaxColumn = UINT_MAX;
    else if (Style.ColumnLimit >= Changes[i].TokenLength)
      ChangeMaxColumn = Style.ColumnLimit - Changes[i].TokenLength;
    else
      ChangeMaxColumn = ChangeMinColumn;

    // If we don't create a replacement for this change, we have to consider
    // it to be immovable.
    if (!Changes[i].CreateReplacement)
      ChangeMaxColumn = ChangeMinColumn;

    if (i + 1 != e && Changes[i + 1].ContinuesPPDirective)
      ChangeMaxColumn -= 2;
    // If this comment follows an } in column 0, it probably documents the
    // closing of a namespace and we don't want to align it.
    bool FollowsRBraceInColumn0 = i > 0 && Changes[i].NewlinesBefore == 0 &&
                                  Changes[i - 1].Tok->is(tok::r_brace) &&
                                  Changes[i - 1].StartOfTokenColumn == 0;
    bool WasAlignedWithStartOfNextLine = false;
    if (Changes[i].NewlinesBefore == 1) { // A comment on its own line.
      unsigned CommentColumn = SourceMgr.getSpellingColumnNumber(
          Changes[i].OriginalWhitespaceRange.getEnd());
      for (unsigned j = i + 1; j != e; ++j) {
        if (Changes[j].Tok->is(tok::comment))
          continue;

        unsigned NextColumn = SourceMgr.getSpellingColumnNumber(
            Changes[j].OriginalWhitespaceRange.getEnd());
        // The start of the next token was previously aligned with the
        // start of this comment.
        WasAlignedWithStartOfNextLine =
            CommentColumn == NextColumn ||
            CommentColumn == NextColumn + Style.IndentWidth;
        break;
      }
    }
    if (!Style.AlignTrailingComments || FollowsRBraceInColumn0) {
      alignTrailingComments(StartOfSequence, i, MinColumn);
      MinColumn = ChangeMinColumn;
      MaxColumn = ChangeMinColumn;
      StartOfSequence = i;
    } else if (BreakBeforeNext || Newlines > 1 ||
               (ChangeMinColumn > MaxColumn || ChangeMaxColumn < MinColumn) ||
               // Break the comment sequence if the previous line did not end
               // in a trailing comment.
               (Changes[i].NewlinesBefore == 1 && i > 0 &&
                !Changes[i - 1].IsTrailingComment) ||
               WasAlignedWithStartOfNextLine) {
      alignTrailingComments(StartOfSequence, i, MinColumn);
      MinColumn = ChangeMinColumn;
      MaxColumn = ChangeMaxColumn;
      StartOfSequence = i;
    } else {
      MinColumn = std::max(MinColumn, ChangeMinColumn);
      MaxColumn = std::min(MaxColumn, ChangeMaxColumn);
    }
    BreakBeforeNext = (i == 0) || (Changes[i].NewlinesBefore > 1) ||
                      // Never start a sequence with a comment at the beginning
                      // of the line.
                      (Changes[i].NewlinesBefore == 1 && StartOfSequence == i);
    Newlines = 0;
  }
  alignTrailingComments(StartOfSequence, Changes.size(), MinColumn);
}

void WhitespaceManager::alignTrailingComments(unsigned Start, unsigned End,
                                              unsigned Column) {
  for (unsigned i = Start; i != End; ++i) {
    int Shift = 0;
    if (Changes[i].IsTrailingComment) {
      Shift = Column - Changes[i].StartOfTokenColumn;
    }
    if (Changes[i].StartOfBlockComment) {
      Shift = Changes[i].IndentationOffset +
              Changes[i].StartOfBlockComment->StartOfTokenColumn -
              Changes[i].StartOfTokenColumn;
    }
    assert(Shift >= 0);
    Changes[i].Spaces += Shift;
    if (i + 1 != Changes.size())
      Changes[i + 1].PreviousEndOfTokenColumn += Shift;
    Changes[i].StartOfTokenColumn += Shift;
  }
}

void WhitespaceManager::alignEscapedNewlines() {
  if (Style.AlignEscapedNewlines == FormatStyle::ENAS_DontAlign)
    return;

  bool AlignLeft = Style.AlignEscapedNewlines == FormatStyle::ENAS_Left;
  unsigned MaxEndOfLine = AlignLeft ? 0 : Style.ColumnLimit;
  unsigned StartOfMacro = 0;
  for (unsigned i = 1, e = Changes.size(); i < e; ++i) {
    Change &C = Changes[i];
    if (C.NewlinesBefore > 0) {
      if (C.ContinuesPPDirective) {
        MaxEndOfLine = std::max(C.PreviousEndOfTokenColumn + 2, MaxEndOfLine);
      } else {
        alignEscapedNewlines(StartOfMacro + 1, i, MaxEndOfLine);
        MaxEndOfLine = AlignLeft ? 0 : Style.ColumnLimit;
        StartOfMacro = i;
      }
    }
  }
  alignEscapedNewlines(StartOfMacro + 1, Changes.size(), MaxEndOfLine);
}

void WhitespaceManager::alignEscapedNewlines(unsigned Start, unsigned End,
                                             unsigned Column) {
  for (unsigned i = Start; i < End; ++i) {
    Change &C = Changes[i];
    if (C.NewlinesBefore > 0) {
      assert(C.ContinuesPPDirective);
      if (C.PreviousEndOfTokenColumn + 1 > Column)
        C.EscapedNewlineColumn = 0;
      else
        C.EscapedNewlineColumn = Column;
    }
  }
}

void WhitespaceManager::generateChanges() {
  for (unsigned i = 0, e = Changes.size(); i != e; ++i) {
    const Change &C = Changes[i];
    if (i > 0) {
      assert(Changes[i - 1].OriginalWhitespaceRange.getBegin() !=
                 C.OriginalWhitespaceRange.getBegin() &&
             "Generating two replacements for the same location");
    }
    if (C.CreateReplacement) {
      std::string ReplacementText = C.PreviousLinePostfix;
      if (C.ContinuesPPDirective)
        appendEscapedNewlineText(ReplacementText, C.NewlinesBefore,
                                 C.PreviousEndOfTokenColumn,
                                 C.EscapedNewlineColumn);
      else
        appendNewlineText(ReplacementText, C.NewlinesBefore);
      appendIndentText(
          ReplacementText, C.Tok->IndentLevel, std::max(0, C.Spaces),
          C.StartOfTokenColumn - std::max(0, C.Spaces), C.IsAligned);
      ReplacementText.append(C.CurrentLinePrefix);
      storeReplacement(C.OriginalWhitespaceRange, ReplacementText);
    }
  }
}

void WhitespaceManager::storeReplacement(SourceRange Range, StringRef Text) {
  unsigned WhitespaceLength = SourceMgr.getFileOffset(Range.getEnd()) -
                              SourceMgr.getFileOffset(Range.getBegin());
  // Don't create a replacement, if it does not change anything.
  if (StringRef(SourceMgr.getCharacterData(Range.getBegin()),
                WhitespaceLength) == Text)
    return;
  auto Err = Replaces.add(tooling::Replacement(
      SourceMgr, CharSourceRange::getCharRange(Range), Text));
  // FIXME: better error handling. For now, just print an error message in the
  // release version.
  if (Err) {
    llvm::errs() << llvm::toString(std::move(Err)) << "\n";
    assert(false);
  }
}

void WhitespaceManager::appendNewlineText(std::string &Text,
                                          unsigned Newlines) {
  for (unsigned i = 0; i < Newlines; ++i)
    Text.append(UseCRLF ? "\r\n" : "\n");
}

void WhitespaceManager::appendEscapedNewlineText(
    std::string &Text, unsigned Newlines, unsigned PreviousEndOfTokenColumn,
    unsigned EscapedNewlineColumn) {
  if (Newlines > 0) {
    unsigned Spaces =
        std::max<int>(1, EscapedNewlineColumn - PreviousEndOfTokenColumn - 1);
    for (unsigned i = 0; i < Newlines; ++i) {
      Text.append(Spaces, ' ');
      Text.append(UseCRLF ? "\\\r\n" : "\\\n");
      Spaces = std::max<int>(0, EscapedNewlineColumn - 1);
    }
  }
}

void WhitespaceManager::appendIndentText(std::string &Text,
                                         unsigned IndentLevel, unsigned Spaces,
                                         unsigned WhitespaceStartColumn,
                                         bool IsAligned) {
  switch (Style.UseTab) {
  case FormatStyle::UT_Never:
    Text.append(Spaces, ' ');
    break;
  case FormatStyle::UT_Always: {
    if (Style.TabWidth) {
      unsigned FirstTabWidth =
          Style.TabWidth - WhitespaceStartColumn % Style.TabWidth;

      // Insert only spaces when we want to end up before the next tab.
      if (Spaces < FirstTabWidth || Spaces == 1) {
        Text.append(Spaces, ' ');
        break;
      }
      // Align to the next tab.
      Spaces -= FirstTabWidth;
      Text.append("\t");

      Text.append(Spaces / Style.TabWidth, '\t');
      Text.append(Spaces % Style.TabWidth, ' ');
    } else if (Spaces == 1) {
      Text.append(Spaces, ' ');
    }
    break;
  }
  case FormatStyle::UT_ForIndentation:
    if (WhitespaceStartColumn == 0) {
      unsigned Indentation = IndentLevel * Style.IndentWidth;
      Spaces = appendTabIndent(Text, Spaces, Indentation);
    }
    Text.append(Spaces, ' ');
    break;
  case FormatStyle::UT_ForContinuationAndIndentation:
    if (WhitespaceStartColumn == 0)
      Spaces = appendTabIndent(Text, Spaces, Spaces);
    Text.append(Spaces, ' ');
    break;
  case FormatStyle::UT_AlignWithSpaces:
    if (WhitespaceStartColumn == 0) {
      unsigned Indentation =
          IsAligned ? IndentLevel * Style.IndentWidth : Spaces;
      Spaces = appendTabIndent(Text, Spaces, Indentation);
    }
    Text.append(Spaces, ' ');
    break;
  }
}

unsigned WhitespaceManager::appendTabIndent(std::string &Text, unsigned Spaces,
                                            unsigned Indentation) {
  // This happens, e.g. when a line in a block comment is indented less than the
  // first one.
  if (Indentation > Spaces)
    Indentation = Spaces;
  if (Style.TabWidth) {
    unsigned Tabs = Indentation / Style.TabWidth;
    Text.append(Tabs, '\t');
    Spaces -= Tabs * Style.TabWidth;
  }
  return Spaces;
}

} // namespace format
} // namespace clang
