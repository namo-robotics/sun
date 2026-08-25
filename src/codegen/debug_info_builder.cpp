// debug_info_builder.cpp — DWARF debug metadata emission for -g builds

#include "codegen/debug_info_builder.h"

#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/DerivedTypes.h>

#include <filesystem>
#include <set>

namespace sun {

using namespace llvm;

DebugInfoBuilder::DebugInfoBuilder(llvm::Module* module, bool enabled)
    : module_(module) {
  if (!enabled || !module) return;
  di_ = std::make_unique<DIBuilder>(*module);
  if (!module->getModuleFlag("Debug Info Version")) {
    module->addModuleFlag(Module::Warning, "Debug Info Version",
                          DEBUG_METADATA_VERSION);
  }
  if (!module->getModuleFlag("Dwarf Version")) {
    module->addModuleFlag(Module::Warning, "Dwarf Version", 5);
  }
}

DICompileUnit* DebugInfoBuilder::ensureCompileUnit(
    const std::optional<std::string>& hint) {
  if (cu_) return cu_;
  DIFile* file;
  if (hint && !hint->empty()) {
    file = getFile(hint);
  } else {
    file = di_->createFile(module_->getModuleIdentifier(), ".");
  }
  cu_ = di_->createCompileUnit(dwarf::DW_LANG_C_plus_plus_14, file, "sun",
                               /*isOptimized=*/false, /*Flags=*/"",
                               /*RuntimeVersion=*/0);
  return cu_;
}

DIFile* DebugInfoBuilder::getFile(const std::optional<std::string>& path) {
  if (!di_) return nullptr;
  if (!path || path->empty()) {
    // Fall back to the compile unit's file for locations with no source path
    // (string-evaluated sources, synthesized nodes).
    return ensureCompileUnit(std::nullopt)->getFile();
  }
  auto it = fileCache_.find(*path);
  if (it != fileCache_.end()) return it->second;
  std::filesystem::path p(*path);
  DIFile* file = di_->createFile(
      p.filename().string(),
      p.has_parent_path() ? p.parent_path().string() : std::string("."));
  fileCache_[*path] = file;
  return file;
}

DISubprogram* DebugInfoBuilder::enterFunction(llvm::IRBuilderBase& builder,
                                              llvm::Function* func,
                                              const std::string& name,
                                              const Position& loc) {
  if (!di_ || !func) return nullptr;
  ensureCompileUnit(loc.filePath);
  DIFile* file = getFile(loc.filePath);
  auto* spType = di_->createSubroutineType(di_->getOrCreateTypeArray({}));
  auto* sp =
      di_->createFunction(file, name, func->getName(), file, loc.line, spType,
                          /*ScopeLine=*/loc.line, DINode::FlagPrototyped,
                          DISubprogram::SPFlagDefinition);
  func->setSubprogram(sp);
  scopeStack_.push_back(sp);
  // Drop any location inherited from the enclosing function's codegen
  builder.SetCurrentDebugLocation(DebugLoc());
  return sp;
}

void DebugInfoBuilder::exitFunction(llvm::Function* func) {
  if (!di_ || !func) return;
  auto* sp = func->getSubprogram();
  if (!sp) return;
  while (!scopeStack_.empty()) {
    auto* top = scopeStack_.back();
    scopeStack_.pop_back();
    if (top == sp) break;
  }
  di_->finalizeSubprogram(sp);
}

bool DebugInfoBuilder::pushLexicalBlock(llvm::IRBuilderBase& builder,
                                        const Position& loc) {
  if (!di_ || scopeStack_.empty() || !builder.GetInsertBlock()) return false;
  auto* parent = dyn_cast<DILocalScope>(scopeStack_.back());
  auto* sp = builder.GetInsertBlock()->getParent()->getSubprogram();
  // Only nest under the subprogram currently being emitted; the stack top can
  // belong to another function around nested function codegen.
  if (!parent || !sp || parent->getSubprogram() != sp) return false;
  scopeStack_.push_back(di_->createLexicalBlock(parent, getFile(loc.filePath),
                                                loc.line, loc.column));
  return true;
}

void DebugInfoBuilder::popLexicalBlock() {
  if (!di_ || scopeStack_.empty()) return;
  if (isa<DILexicalBlock>(scopeStack_.back())) scopeStack_.pop_back();
}

void DebugInfoBuilder::attachExpressionLocation(llvm::IRBuilderBase& builder,
                                                const Position& loc) {
  if (!di_) return;
  auto* block = builder.GetInsertBlock();
  llvm::Function* func = block ? block->getParent() : nullptr;
  DISubprogram* sp = func ? func->getSubprogram() : nullptr;
  if (!sp) {
    // Functions without a subprogram must carry no locations — including any
    // stale one inherited from a previously emitted function.
    builder.SetCurrentDebugLocation(DebugLoc());
    return;
  }
  DIScope* scope = currentLocalScope(func);
  builder.SetCurrentDebugLocation(
      DILocation::get(module_->getContext(), loc.line, loc.column, scope));
}

void DebugInfoBuilder::clearLocation(llvm::IRBuilderBase& builder) {
  if (!di_) return;
  builder.SetCurrentDebugLocation(DebugLoc());
}

DILocalScope* DebugInfoBuilder::currentLocalScope(llvm::Function* func) const {
  auto* sp = func ? func->getSubprogram() : nullptr;
  if (!sp) return nullptr;
  for (auto it = scopeStack_.rbegin(); it != scopeStack_.rend(); ++it) {
    if (auto* local = dyn_cast<DILocalScope>(*it);
        local && local->getSubprogram() == sp) {
      return local;
    }
  }
  return sp;
}

void DebugInfoBuilder::declareVariable(llvm::IRBuilderBase& builder,
                                       llvm::AllocaInst* alloca,
                                       llvm::DILocalVariable* var,
                                       const Position& loc,
                                       llvm::DILocalScope* scope) {
  di_->insertDeclare(
      alloca, var, di_->createExpression(),
      DILocation::get(module_->getContext(), loc.line, loc.column, scope),
      builder.GetInsertBlock());
}

void DebugInfoBuilder::declareParameter(llvm::IRBuilderBase& builder,
                                        llvm::AllocaInst* alloca,
                                        const std::string& name,
                                        const TypePtr& type,
                                        const Position& loc, unsigned argNo,
                                        bool artificial) {
  if (!di_ || !alloca || !builder.GetInsertBlock()) return;
  auto* scope = currentLocalScope(builder.GetInsertBlock()->getParent());
  if (!scope) return;
  auto* var = di_->createParameterVariable(
      scope, name, argNo, getFile(loc.filePath), loc.line, resolveType(type),
      /*AlwaysPreserve=*/true,
      artificial ? DINode::FlagArtificial | DINode::FlagObjectPointer
                 : DINode::FlagZero);
  declareVariable(builder, alloca, var, loc, scope);
}

void DebugInfoBuilder::declareThisParameter(llvm::IRBuilderBase& builder,
                                            llvm::AllocaInst* alloca,
                                            const TypePtr& classType) {
  if (!di_ || !alloca || !builder.GetInsertBlock()) return;
  auto* scope = currentLocalScope(builder.GetInsertBlock()->getParent());
  if (!scope) return;
  auto* var = di_->createParameterVariable(
      scope, "this", 1, ensureCompileUnit(std::nullopt)->getFile(), 0,
      pointerTo(resolveType(classType)),
      /*AlwaysPreserve=*/true,
      DINode::FlagArtificial | DINode::FlagObjectPointer);
  declareVariable(builder, alloca, var, Position(0, 0), scope);
}

void DebugInfoBuilder::declareLocal(llvm::IRBuilderBase& builder,
                                    llvm::AllocaInst* alloca,
                                    const std::string& name,
                                    const TypePtr& type, const Position& loc) {
  if (!di_ || !alloca || !builder.GetInsertBlock()) return;
  auto* scope = currentLocalScope(builder.GetInsertBlock()->getParent());
  if (!scope) return;
  auto* var =
      di_->createAutoVariable(scope, name, getFile(loc.filePath), loc.line,
                              resolveType(type), /*AlwaysPreserve=*/true);
  declareVariable(builder, alloca, var, loc, scope);
}

DIType* DebugInfoBuilder::pointerTo(DIType* pointee) {
  return di_->createPointerType(
      pointee, module_->getDataLayout().getPointerSizeInBits());
}

DIType* DebugInfoBuilder::structFor(
    const std::string& name, llvm::StructType* st,
    const std::vector<std::pair<std::string, DIType*>>& members) {
  const auto& dl = module_->getDataLayout();
  if (!st || !st->isSized() || members.size() > st->getNumElements()) {
    return di_->createUnspecifiedType(name);
  }
  const auto* layout = dl.getStructLayout(st);
  DIFile* file = ensureCompileUnit(std::nullopt)->getFile();
  SmallVector<Metadata*, 8> elems;
  // Forward-declare so member types may refer back to this struct.
  auto* fwd = di_->createReplaceableCompositeType(dwarf::DW_TAG_structure_type,
                                                  name, cu_, file, 0);
  for (size_t i = 0; i < members.size(); ++i) {
    llvm::Type* elemTy = st->getElementType(i);
    elems.push_back(di_->createMemberType(
        fwd, members[i].first, file, 0, dl.getTypeSizeInBits(elemTy),
        dl.getABITypeAlign(elemTy).value() * 8,
        layout->getElementOffsetInBits(i), DINode::FlagZero,
        members[i].second));
  }
  auto* full = di_->createStructType(
      cu_, name, file, 0, dl.getTypeSizeInBits(st),
      dl.getABITypeAlign(st).value() * 8, DINode::FlagZero,
      /*DerivedFrom=*/nullptr, di_->getOrCreateArray(elems));
  di_->replaceTemporary(llvm::TempDIType(fwd), full);
  // Per-function verification runs mid-codegen, before DIBuilder::finalize()
  // would resolve temporary-node cycles — resolve eagerly.
  if (!full->isResolved()) full->resolveCycles();
  return full;
}

DIType* DebugInfoBuilder::resolveType(const TypePtr& type) {
  if (!di_) return nullptr;
  if (!type) return di_->createUnspecifiedType("unknown");
  auto it = typeCache_.find(type.get());
  if (it != typeCache_.end()) return it->second;
  auto* resolved = resolveTypeImpl(*type);
  typeCache_[type.get()] = resolved;
  return resolved;
}

DIType* DebugInfoBuilder::resolveTypeImpl(const Type& type) {
  auto& ctx = module_->getContext();
  const auto& dl = module_->getDataLayout();
  ensureCompileUnit(std::nullopt);

  auto basicInt = [&](const Type& t) {
    unsigned bits = 32;
    switch (t.getKind()) {
      case Type::Kind::Int8:
      case Type::Kind::UInt8:
        bits = 8;
        break;
      case Type::Kind::Int16:
      case Type::Kind::UInt16:
        bits = 16;
        break;
      case Type::Kind::Int32:
      case Type::Kind::UInt32:
        bits = 32;
        break;
      case Type::Kind::Int64:
      case Type::Kind::UInt64:
        bits = 64;
        break;
      default:
        break;
    }
    // Signedness only exists in sun::Type; LLVM integers are signless.
    return di_->createBasicType(
        t.toString(), bits,
        t.isSigned() ? dwarf::DW_ATE_signed : dwarf::DW_ATE_unsigned);
  };

  switch (type.getKind()) {
    case Type::Kind::Void:
      return nullptr;  // DWARF spells void as an absent type
    case Type::Kind::Bool:
      return di_->createBasicType("bool", 8, dwarf::DW_ATE_boolean);
    case Type::Kind::Int8:
    case Type::Kind::Int16:
    case Type::Kind::Int32:
    case Type::Kind::Int64:
    case Type::Kind::UInt8:
    case Type::Kind::UInt16:
    case Type::Kind::UInt32:
    case Type::Kind::UInt64:
      return basicInt(type);
    case Type::Kind::Float32:
      return di_->createBasicType("f32", 32, dwarf::DW_ATE_float);
    case Type::Kind::Float64:
      return di_->createBasicType("f64", 64, dwarf::DW_ATE_float);
    case Type::Kind::Char:
      return di_->createBasicType("char", 32, dwarf::DW_ATE_UTF);

    case Type::Kind::Reference: {
      const auto& ref = static_cast<const ReferenceType&>(type);
      return di_->createReferenceType(dwarf::DW_TAG_reference_type,
                                      resolveType(ref.getReferencedType()),
                                      dl.getPointerSizeInBits());
    }
    case Type::Kind::RawPointer: {
      const auto& ptr = static_cast<const RawPointerType&>(type);
      return pointerTo(resolveType(ptr.getPointeeType()));
    }
    case Type::Kind::NullPointer:
      return pointerTo(nullptr);

    case Type::Kind::StaticPointer: {
      const auto& sp = static_cast<const StaticPointerType&>(type);
      auto* st = dyn_cast<llvm::StructType>(sp.toLLVMType(ctx));
      return structFor(sp.toString(), st,
                       {{"data", pointerTo(resolveType(sp.getPointeeType()))},
                        {"length", di_->createBasicType(
                                       "u64", 64, dwarf::DW_ATE_unsigned)}});
    }

    case Type::Kind::Enum: {
      const auto& et = static_cast<const EnumType&>(type);
      SmallVector<Metadata*, 8> variants;
      std::set<std::string> seen;
      for (const auto& v : et.getVariants()) {
        if (!seen.insert(v.name).second) continue;
        variants.push_back(di_->createEnumerator(v.name, v.value));
      }
      auto* tagDI = di_->createEnumerationType(
          cu_, et.getDisplayName(), cu_->getFile(), 0, 32, 32,
          di_->getOrCreateArray(variants),
          di_->createBasicType("i32", 32, dwarf::DW_ATE_signed));
      if (!et.hasPayload()) return tagDI;

      // Payload enum: describe the storage struct { tag, payload bytes }.
      // A proper DW_TAG_variant_part description is future work.
      auto* st = et.cachedStorageType;
      if (!st || !st->isSized()) {
        return di_->createUnspecifiedType(et.getDisplayName());
      }
      llvm::Type* payloadTy = st->getElementType(1);
      auto* byteDI = di_->createBasicType("u8", 8, dwarf::DW_ATE_unsigned_char);
      auto* payloadDI = di_->createArrayType(
          dl.getTypeSizeInBits(payloadTy),
          dl.getABITypeAlign(payloadTy).value() * 8, byteDI,
          di_->getOrCreateArray(
              {di_->getOrCreateSubrange(0, dl.getTypeAllocSize(payloadTy))}));
      return structFor(et.getDisplayName(), st,
                       {{"tag", tagDI}, {"payload", payloadDI}});
    }

    case Type::Kind::Class: {
      const auto& ct = static_cast<const ClassType&>(type);
      auto* st = dyn_cast<llvm::StructType>(ct.toLLVMType(ctx));
      if (!st || !st->isSized()) {
        return di_->createUnspecifiedType(ct.getDisplayName());
      }
      // Break field-type recursion: cache a forward declaration first.
      DIFile* file = cu_->getFile();
      auto* fwd = di_->createReplaceableCompositeType(
          dwarf::DW_TAG_structure_type, ct.getDisplayName(), cu_, file, 0);
      typeCache_[&type] = fwd;
      const auto* layout = dl.getStructLayout(st);
      SmallVector<Metadata*, 8> elems;
      for (const auto& field : ct.getFields()) {
        if (field.index >= st->getNumElements()) continue;
        llvm::Type* elemTy = st->getElementType(field.index);
        elems.push_back(di_->createMemberType(
            fwd, field.name, file, 0, dl.getTypeSizeInBits(elemTy),
            dl.getABITypeAlign(elemTy).value() * 8,
            layout->getElementOffsetInBits(field.index), DINode::FlagZero,
            resolveType(field.type)));
      }
      auto* full = di_->createStructType(
          cu_, ct.getDisplayName(), file, 0, dl.getTypeSizeInBits(st),
          dl.getABITypeAlign(st).value() * 8, DINode::FlagZero,
          /*DerivedFrom=*/nullptr, di_->getOrCreateArray(elems));
      di_->replaceTemporary(llvm::TempDIType(fwd), full);
      // Resolve eagerly: per-function verification runs before finalize()
      if (!full->isResolved()) full->resolveCycles();
      typeCache_[&type] = full;
      return full;
    }

    case Type::Kind::Array: {
      const auto& at = static_cast<const sun::ArrayType&>(type);
      auto* st = ArrayType::getArrayStructType(ctx);
      auto* u64 = di_->createBasicType("u64", 64, dwarf::DW_ATE_unsigned);
      return structFor(
          "array<" + at.getElementType()->toString() + ">", st,
          {{"data", pointerTo(resolveType(at.getElementType()))},
           {"ndims", di_->createBasicType("i32", 32, dwarf::DW_ATE_signed)},
           {"dims", pointerTo(u64)}});
    }

    case Type::Kind::Slice: {
      auto* st = dyn_cast<llvm::StructType>(type.toLLVMType(ctx));
      auto* i64 = di_->createBasicType("i64", 64, dwarf::DW_ATE_signed);
      return structFor("slice", st, {{"start", i64}, {"end", i64}});
    }

    case Type::Kind::ErrorUnion: {
      const auto& eu = static_cast<const ErrorUnionType&>(type);
      auto* st = dyn_cast<llvm::StructType>(eu.toLLVMType(ctx));
      return structFor(
          eu.toString(), st,
          {{"is_error", di_->createBasicType("bool", 8, dwarf::DW_ATE_boolean)},
           {"value", resolveType(eu.getValueType())}});
    }

    case Type::Kind::Function:
    case Type::Kind::Lambda: {
      // Callable values are closure structs { ptr func, ptr env }.
      auto* st = llvm::StructType::get(
          ctx, {PointerType::getUnqual(ctx), PointerType::getUnqual(ctx)});
      return structFor(
          type.toString(), st,
          {{"func", pointerTo(nullptr)}, {"env", pointerTo(nullptr)}});
    }

    default: {
      // Interfaces, threads, modules, type parameters: opaque but legal.
      return di_->createUnspecifiedType(type.toString());
    }
  }
}

void DebugInfoBuilder::finalize() {
  if (!di_ || finalized_) return;
  di_->finalize();
  finalized_ = true;
}

void DebugInfoBuilder::stripFromModule(llvm::Module& module) {
  llvm::StripDebugInfo(module);
  // StripDebugInfo leaves module flags; rebuild them without the debug ones.
  auto* flags = module.getModuleFlagsMetadata();
  if (!flags) return;
  SmallVector<Module::ModuleFlagEntry, 8> entries;
  module.getModuleFlagsMetadata(entries);
  flags->eraseFromParent();
  for (const auto& entry : entries) {
    StringRef key = entry.Key->getString();
    if (key == "Debug Info Version" || key == "Dwarf Version") continue;
    module.addModuleFlag(entry.Behavior, key, entry.Val);
  }
}

}  // namespace sun
