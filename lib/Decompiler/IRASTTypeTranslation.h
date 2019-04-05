#ifndef REVNGC_IRASTTYPETRANSLATION_H
#define REVNGC_IRASTTYPETRANSLATION_H

// std includes
#include <map>

// LLVM includes
#include <llvm/ADT/Optional.h>
#include <llvm/ADT/SmallVector.h>

// clang includes
#include <clang/AST/Type.h>

namespace llvm {
class GlobalVariable;
class Type;
class Value;
} // end namespace llvm

namespace clang {
class FieldDecl;
class TypeDecl;
} // end namespace clang

namespace IRASTTypeTranslation {

using TypeDeclMap = std::map<const llvm::Type *, clang::TypeDecl *>;
using FieldDeclMap = std::map<clang::TypeDecl *,
                              llvm::SmallVector<clang::FieldDecl *, 8>>;

clang::QualType getOrCreateBoolQualType(const llvm::Type *Ty,
                                        clang::ASTContext &ASTCtx,
                                        TypeDeclMap &TypeDecls);

clang::QualType getOrCreateQualType(const llvm::Value *I,
                                    clang::ASTContext &ASTCtx,
                                    clang::DeclContext &DeclCtx,
                                    TypeDeclMap &TypeDecls,
                                    FieldDeclMap &FieldDecls);

clang::QualType getOrCreateQualType(const llvm::GlobalVariable *G,
                                    clang::ASTContext &ASTCtx,
                                    clang::DeclContext &DeclCtx,
                                    TypeDeclMap &TypeDecls,
                                    FieldDeclMap &FieldDecls);

clang::QualType getOrCreateQualType(const llvm::Type *T,
                                    clang::ASTContext &ASTCtx,
                                    clang::DeclContext &DeclCtx,
                                    TypeDeclMap &TypeDecls,
                                    FieldDeclMap &FieldDecls);

} // end namespace IRASTTypeTranslation

#endif // REVNGC_IRASTTYPETRANSLATION_H
