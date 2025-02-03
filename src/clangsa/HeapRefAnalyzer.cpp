#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"

#include "clang/AST/DeclCXX.h"
#include "clang/AST/ASTContext.h"
#include "clang/Lex/Lexer.h"         // For Lexer::getSourceText
#include "clang/Basic/SourceManager.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang;
using namespace ento;
using namespace ast_matchers;

//===----------------------------------------------------------------------===//
// Checker Class Declaration
//===----------------------------------------------------------------------===//

namespace clang {
namespace ento {

class HeapRefAnalyzerChecker : public Checker<check::ASTDecl<RecordDecl>> {
public:
  void checkASTDecl(const RecordDecl *RD, AnalysisManager &Mgr, BugReporter &BR) const;

private:
  mutable std::unique_ptr<BugType> HeapRefBug;

  void reportError(const FieldDecl *Field, const RecordDecl *RD,
                   BugReporter &BR, llvm::StringRef Description) const;

  static bool isJuliaHeapPointer(QualType PointeeType, ASTContext &Ctx);
};

} // end namespace ento
} // end namespace clang

//===----------------------------------------------------------------------===//
// Helpers
//===----------------------------------------------------------------------===//

/// Check raw source for "JL_DATA_TYPE" in the declaration of \p RD.
static bool hasJL_DATA_TYPEMacro(const RecordDecl *RD, ASTContext &Ctx) {
  if (!RD->isThisDeclarationADefinition())
    return false;

  SourceRange Range = RD->getSourceRange();
  if (!Range.isValid())
    return false;

  const SourceManager &SM = Ctx.getSourceManager();
  bool Invalid = false;

  // Extract the exact source text of this record's definition
  StringRef SourceText = Lexer::getSourceText(
      CharSourceRange::getCharRange(Range),
      SM, Ctx.getLangOpts(), &Invalid
  );

  // If we successfully extracted text, search for "JL_DATA_TYPE"
  return (!Invalid && SourceText.contains("JL_DATA_TYPE"));
}

//===----------------------------------------------------------------------===//
// reportError
//===----------------------------------------------------------------------===//

void clang::ento::HeapRefAnalyzerChecker::reportError(
    const FieldDecl *Field,
    const RecordDecl *RD,
    BugReporter &BR,
    llvm::StringRef Description) const
{
  if (!HeapRefBug) {
    HeapRefBug = std::make_unique<BugType>(
        this, "Heap Reference", "Julia GC Safety"
    );
  }

  PathDiagnosticLocation Loc =
      PathDiagnosticLocation::createBegin(Field, BR.getSourceManager());

  std::string fullDescription =
      (Description + " (in struct " + RD->getQualifiedNameAsString() + ")").str();

  BR.EmitBasicReport(
      RD,                            // Associated Decl
      this,                          // Checker
      "Heap Reference Detected",     // Short description
      "Julia GC Safety",             // Category
      fullDescription,               // Longer explanation
      Loc                            // Source location
  );
}

//===----------------------------------------------------------------------===//
// checkASTDecl
//===----------------------------------------------------------------------===//

/// Determine if a type points to Julia heap memory.
bool clang::ento::HeapRefAnalyzerChecker::isJuliaHeapPointer(QualType PointeeType, ASTContext &Ctx) {
  if (const auto *PointeeRD = PointeeType->getAsRecordDecl()) {
    // Check if the pointee is jl_value_t or jl_datatype_t.
    if (PointeeRD->getQualifiedNameAsString() == "jl_value_t" ||
        PointeeRD->getQualifiedNameAsString() == "jl_datatype_t") {
      return true;
    }

    // Check if the pointee has the JL_DATA_TYPE macro.
    if (hasJL_DATA_TYPEMacro(PointeeRD, Ctx)) {
      return true;
    }
  }

  return false;
}

void clang::ento::HeapRefAnalyzerChecker::checkASTDecl(
    const RecordDecl *RD,
    AnalysisManager &Mgr,
    BugReporter &BR) const
{
  printf("Examine "); RD->dump(); printf("\n");

  // Only analyze if we have a definition.
  if (!RD->isThisDeclarationADefinition())
    return;

  // 0. Check file path to see if it's in "src/" (or however you define "Julia code").
  const auto &SM = Mgr.getASTContext().getSourceManager();
  SourceLocation Loc = RD->getLocation();
  if (!Loc.isValid())
    return;

  StringRef FileName = SM.getFilename(SM.getSpellingLoc(Loc));
  // e.g., if your Julia path is "/home/yilin/Code/julia_workspace/julia/src/"
  // or you just want to see if the file is inside "src/" somewhere:
  if (FileName.contains("include")) {
    // This means we are not inside Julia’s source tree.
    return;
  }

  // 1. Skip this type entirely if it has JL_DATA_TYPE in its definition.
  if (hasJL_DATA_TYPEMacro(RD, Mgr.getASTContext())) {
    // This is effectively the "JL_DATA_TYPE" type we want to skip analyzing.
    return;
  }

  if (RD->isUnion()) {
    printf("IsUnion\n");
    for (const auto *FieldInUnion : RD->fields()) {
      FieldInUnion->dump();
      if (FieldInUnion->getType()->isPointerType()) {
        printf("IsPointerType\n");
        if (isJuliaHeapPointer(FieldInUnion->getType()->getPointeeType(), Mgr.getASTContext()) && FieldInUnion->getName() == "_ref") {
          // Skip checks for fields matching the pattern of jl_gc_root.
          printf("Skip the union that includes a _ref field");
          return;
        }
      }
    }
  }

  // 2. Inspect each field for pointer types.
  for (const auto *Field : RD->fields()) {
    QualType FieldType = Field->getType();

    // If this is GC traced, it is fine. Ignore it.
    if (Field->hasAttr<AnnotateAttr>()) {
      for (const auto *Attr : Field->specific_attrs<AnnotateAttr>()) {
        printf("Attr: %s\n", Attr->getAnnotation());
        if (Attr->getAnnotation() == "julia_gc_root_promise_traced") {
          continue;
        }
      }
    }

      if (const RecordType *RT = Field->getType()->getAs<RecordType>()) {
        if (RT->getDecl()->isUnion() && RT->getDecl()->getName() == "jl_gc_root") {
          continue;
        }
      }

    // Skip jl_gc_root_ref types
    if (const TypedefType *TD = dyn_cast<TypedefType>(FieldType)) {
      if (TD->getDecl()->getName() == "jl_gc_root") {
        continue;
      }
    }

    if (!FieldType->isPointerType()) {
      // If the field type is _Atomic(jl_value_t*), flag it.
      if (FieldType->isAtomicType() && FieldType->getAs<AtomicType>()->getValueType()->isPointerType() && isJuliaHeapPointer(FieldType->getAs<AtomicType>()->getValueType()->getPointeeType(), Mgr.getASTContext())) {
        reportError(Field, RD, BR, "Struct field is _Atomic(jl_value_t*), references Julia GC heap");
        continue;
      }

      // If the field type is some container (e.g., map) with jl_value_t*, flag it.
      if (FieldType->getAs<TemplateSpecializationType>() != nullptr) {
        const TemplateSpecializationType *TS = FieldType->getAs<TemplateSpecializationType>();
        for (auto Arg : TS->template_arguments()) {
          if (Arg.getKind() == TemplateArgument::Type && Arg.getAsType()->isPointerType()) {
            if (isJuliaHeapPointer(Arg.getAsType()->getPointeeType(), Mgr.getASTContext())) {
              reportError(Field, RD, BR, "Struct field is a container holding jl_value_t*, references Julia GC heap");
              break;
            }
          }
        }
      }

      continue;
    }

    QualType PointeeType = FieldType->getPointeeType();

    // Check if this is a Julia heap pointer.
    if (isJuliaHeapPointer(PointeeType, Mgr.getASTContext())) {
      reportError(Field, RD, BR, "Struct field is jl_value_t* or jl_datatype_t*, references Julia GC heap");
    }
  }
}

//===----------------------------------------------------------------------===//
// Registration
//===----------------------------------------------------------------------===//

namespace clang {
namespace ento {

void registerHeapRefAnalyzerChecker(CheckerManager &mgr) {
  mgr.registerChecker<HeapRefAnalyzerChecker>();
}

bool shouldRegisterHeapRefAnalyzerChecker(const CheckerManager &mgr) {
  return true;
}

} // end namespace ento
} // end namespace clang

#ifdef CLANG_PLUGIN
extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<clang::ento::HeapRefAnalyzerChecker>(
      "julia.HeapRefChecker",
      "Detects heap references in Julia runtime structs",
      "https://julialang.org/gc-safety"
  );
}
#endif
