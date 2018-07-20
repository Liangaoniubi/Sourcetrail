#include "data/parser/cxx/CxxAstVisitorComponentIndexer.h"

#include <clang/AST/ASTContext.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Lex/Preprocessor.h>

#include "data/parser/cxx/CanonicalFilePathCache.h"
#include "data/parser/cxx/CxxAstVisitorComponentContext.h"
#include "data/parser/cxx/CxxAstVisitorComponentDeclRefKind.h"
#include "data/parser/cxx/CxxAstVisitorComponentTypeRefKind.h"
#include "data/parser/cxx/utilityClang.h"
#include "data/parser/ParseLocation.h"
#include "data/parser/ParserClient.h"
#include "utility/file/FileRegister.h"

CxxAstVisitorComponentIndexer::CxxAstVisitorComponentIndexer(
	CxxAstVisitor* astVisitor, clang::ASTContext* astContext, std::shared_ptr<ParserClient> client
)
	: CxxAstVisitorComponent(astVisitor)
	, m_astContext(astContext)
	, m_client(client)
{
}

void CxxAstVisitorComponentIndexer::beginTraverseNestedNameSpecifierLoc(const clang::NestedNameSpecifierLoc& loc)
{
	if (!getAstVisitor()->shouldVisitReference(loc.getBeginLoc(), getAstVisitor()->getComponent<CxxAstVisitorComponentContext>()->getTopmostContextDecl()))
	{
		return;
	}

	switch (loc.getNestedNameSpecifier()->getKind())
	{
	case clang::NestedNameSpecifier::Identifier:
		break;
	case clang::NestedNameSpecifier::Namespace:
		{
			const NameHierarchy symbolName = getAstVisitor()->getDeclNameCache()->getValue(loc.getNestedNameSpecifier()->getAsNamespace());
			m_client->recordSymbol(
				symbolName,
				SYMBOL_NAMESPACE,
				ACCESS_NONE,
				DEFINITION_NONE
			);

			m_client->recordQualifierLocation(
				symbolName,
				getParseLocation(loc.getLocalBeginLoc())
			);
		}
		break;
	case clang::NestedNameSpecifier::NamespaceAlias:
		{
			m_client->recordSymbol(
				getAstVisitor()->getDeclNameCache()->getValue(loc.getNestedNameSpecifier()->getAsNamespaceAlias()),
				SYMBOL_NAMESPACE,
				ACCESS_NONE,
				DEFINITION_NONE
			);

			m_client->recordSymbol(
				getAstVisitor()->getDeclNameCache()->getValue(loc.getNestedNameSpecifier()->getAsNamespaceAlias()->getAliasedNamespace()),
				SYMBOL_NAMESPACE,
				ACCESS_NONE,
				DEFINITION_NONE
			);
		}
		break;
	case clang::NestedNameSpecifier::Global:
	case clang::NestedNameSpecifier::Super:
		break;
	case clang::NestedNameSpecifier::TypeSpec:
	case clang::NestedNameSpecifier::TypeSpecWithTemplate:
		if (const clang::CXXRecordDecl* recordDecl = loc.getNestedNameSpecifier()->getAsRecordDecl())
		{
			SymbolKind symbolKind = SYMBOL_KIND_MAX;
			if (recordDecl->isClass())
			{
				symbolKind = SYMBOL_CLASS;
			}
			else if (recordDecl->isStruct())
			{
				symbolKind = SYMBOL_STRUCT;
			}
			else if (recordDecl->isUnion())
			{
				symbolKind = SYMBOL_UNION;
			}

			if (symbolKind != SYMBOL_KIND_MAX)
			{
				const NameHierarchy symbolName = getAstVisitor()->getDeclNameCache()->getValue(recordDecl);
				m_client->recordSymbol(
					symbolName,
					symbolKind,
					ACCESS_NONE,
					DEFINITION_NONE
				);

				m_client->recordQualifierLocation(
					symbolName,
					getParseLocation(loc.getLocalBeginLoc())
				);
			}
		}
		else if (const clang::Type* type = loc.getNestedNameSpecifier()->getAsType())
		{
			m_client->recordQualifierLocation(
				getAstVisitor()->getTypeNameCache()->getValue(type),
				getParseLocation(loc.getLocalBeginLoc())
			);
		}
	}
}

void CxxAstVisitorComponentIndexer::beginTraverseTemplateArgumentLoc(const clang::TemplateArgumentLoc& loc)
{
	if (
		(loc.getArgument().getKind() == clang::TemplateArgument::Template) &&
		(getAstVisitor()->shouldVisitReference(loc.getLocation(), getAstVisitor()->getComponent<CxxAstVisitorComponentContext>()->getTopmostContextDecl()))
	){
		// TODO: maybe move this to VisitTemplateName
		m_client->recordReference(
			getAstVisitor()->getComponent<CxxAstVisitorComponentTypeRefKind>()->getReferenceKind(),
			getAstVisitor()->getDeclNameCache()->getValue(loc.getArgument().getAsTemplate().getAsTemplateDecl()),
			getAstVisitor()->getComponent<CxxAstVisitorComponentContext>()->getContextName(),
			getParseLocation(loc.getLocation())
		);
	}
}

void CxxAstVisitorComponentIndexer::beginTraverseLambdaCapture(clang::LambdaExpr *lambdaExpr, const clang::LambdaCapture *capture)
{
	if ((!lambdaExpr->isInitCapture(capture)) && (capture->capturesVariable()))
	{
		clang::VarDecl* d = capture->getCapturedVar();
		SymbolKind symbolKind = utility::getSymbolKind(d);
		if (symbolKind == SYMBOL_LOCAL_VARIABLE || symbolKind == SYMBOL_PARAMETER)
		{
			if (!d->getNameAsString().empty()) // don't record anonymous parameters
			{
				ParseLocation declLocation = getParseLocation(d->getLocation());
				std::wstring name =
					declLocation.filePath.fileName() + L"<" +
					std::to_wstring(declLocation.startLineNumber) + L":" +
					std::to_wstring(declLocation.startColumnNumber) + L">";
				m_client->recordLocalSymbol(name, getParseLocation(capture->getLocation()));
			}
		}
	}
}

void CxxAstVisitorComponentIndexer::visitTagDecl(clang::TagDecl* d)
{
	if (getAstVisitor()->shouldVisitDecl(d))
	{
		DefinitionKind definitionKind = DEFINITION_NONE;
		if (d->isThisDeclarationADefinition())
		{
			definitionKind = utility::isImplicit(d) ? DEFINITION_IMPLICIT : DEFINITION_EXPLICIT;
		}

		const SymbolKind symbolKind = utility::convertTagKind(d->getTagKind());
		m_client->recordSymbolWithLocationAndScope(
			getAstVisitor()->getDeclNameCache()->getValue(d),
			symbolKind,
			getParseLocation(d->getLocation()),
			getParseLocationOfTagDeclBody(d),
			utility::convertAccessSpecifier(d->getAccess()),
			definitionKind
		);

		if (clang::EnumDecl* enumDecl = clang::dyn_cast_or_null<clang::EnumDecl>(d))
		{
			recordTemplateMemberSpecialization(
				enumDecl->getMemberSpecializationInfo(),
				getAstVisitor()->getDeclNameCache()->getValue(d),
				getParseLocation(d->getLocation()),
				symbolKind
			);
		}
		if(clang::CXXRecordDecl* recordDecl = clang::dyn_cast_or_null<clang::CXXRecordDecl>(d))
		{
			recordTemplateMemberSpecialization(
				recordDecl->getMemberSpecializationInfo(),
				getAstVisitor()->getDeclNameCache()->getValue(d),
				getParseLocation(d->getLocation()),
				symbolKind
			);
		}
	}
}

void CxxAstVisitorComponentIndexer::visitClassTemplateSpecializationDecl(clang::ClassTemplateSpecializationDecl* d)
{
	if (getAstVisitor()->shouldVisitDecl(d))
	{
		clang::NamedDecl* specializedFromDecl = nullptr;

		// todo: use context and childcontext!!
		llvm::PointerUnion<clang::ClassTemplateDecl*, clang::ClassTemplatePartialSpecializationDecl*> pu = d->getSpecializedTemplateOrPartial();
		if (pu.is<clang::ClassTemplateDecl*>())
		{
			specializedFromDecl = pu.get<clang::ClassTemplateDecl*>();
		}
		else if (pu.is<clang::ClassTemplatePartialSpecializationDecl*>())
		{
			specializedFromDecl = pu.get<clang::ClassTemplatePartialSpecializationDecl*>();
		}

		m_client->recordReference(
			REFERENCE_TEMPLATE_SPECIALIZATION,
			getAstVisitor()->getDeclNameCache()->getValue(specializedFromDecl),
			getAstVisitor()->getDeclNameCache()->getValue(d),
			getParseLocation(d->getLocation())
		);
	}
}

void CxxAstVisitorComponentIndexer::visitVarDecl(clang::VarDecl* d)
{
	if (getAstVisitor()->shouldVisitDecl(d))
	{
		SymbolKind symbolKind = utility::getSymbolKind(d);
		if (symbolKind == SYMBOL_LOCAL_VARIABLE || symbolKind == SYMBOL_PARAMETER)
		{
			if (!d->getNameAsString().empty()) // don't record anonymous parameters
			{
				ParseLocation declLocation = getParseLocation(d->getLocation());
				std::wstring name =
					declLocation.filePath.fileName() + L"<" +
					std::to_wstring(declLocation.startLineNumber) + L":" +
					std::to_wstring(declLocation.startColumnNumber) + L">";
				m_client->recordLocalSymbol(name, getParseLocation(d->getLocation()));
			}
		}
		else
		{
			m_client->recordSymbolWithLocation(
				getAstVisitor()->getDeclNameCache()->getValue(d),
				symbolKind,
				getParseLocation(d->getLocation()),
				utility::convertAccessSpecifier(d->getAccess()),
				utility::isImplicit(d) ? DEFINITION_IMPLICIT : DEFINITION_EXPLICIT
			);

			recordTemplateMemberSpecialization(
				d->getMemberSpecializationInfo(),
				getAstVisitor()->getDeclNameCache()->getValue(d),
				getParseLocation(d->getLocation()),
				symbolKind
			);
		}
	}
}

void CxxAstVisitorComponentIndexer::visitVarTemplateSpecializationDecl(clang::VarTemplateSpecializationDecl* d)
{
	if (getAstVisitor()->shouldVisitDecl(d))
	{
		clang::NamedDecl* specializedFromDecl = nullptr;

		// todo: use context and childcontext!!
		llvm::PointerUnion<clang::VarTemplateDecl*, clang::VarTemplatePartialSpecializationDecl*> pu = d->getSpecializedTemplateOrPartial();
		if (pu.is<clang::VarTemplateDecl*>())
		{
			specializedFromDecl = pu.get<clang::VarTemplateDecl*>();
		}
		else if (pu.is<clang::VarTemplatePartialSpecializationDecl*>())
		{
			specializedFromDecl = pu.get<clang::VarTemplatePartialSpecializationDecl*>();
		}

		m_client->recordReference(
			REFERENCE_TEMPLATE_SPECIALIZATION,
			getAstVisitor()->getDeclNameCache()->getValue(specializedFromDecl),
			getAstVisitor()->getDeclNameCache()->getValue(d),
			getParseLocation(d->getLocation())
		);
	}
}

void CxxAstVisitorComponentIndexer::visitFieldDecl(clang::FieldDecl* d)
{
	if (getAstVisitor()->shouldVisitDecl(d))
	{
		m_client->recordSymbolWithLocation(
			getAstVisitor()->getDeclNameCache()->getValue(d),
			SYMBOL_FIELD,
			getParseLocation(d->getLocation()),
			utility::convertAccessSpecifier(d->getAccess()),
			utility::isImplicit(d) ? DEFINITION_IMPLICIT : DEFINITION_EXPLICIT
		);

		if (clang::CXXRecordDecl* declaringRecordDecl = clang::dyn_cast_or_null<clang::CXXRecordDecl>(d->getParent()))
		{
			if (clang::CXXRecordDecl* declaringRecordTemplateDecl = declaringRecordDecl->getTemplateInstantiationPattern())
			{
				for (clang::FieldDecl* templateFieldDecl : declaringRecordTemplateDecl->fields())
				{
					if (d->getName() == templateFieldDecl->getName())
					{
						const NameHierarchy referencedName = getAstVisitor()->getDeclNameCache()->getValue(templateFieldDecl);

						m_client->recordSymbol(referencedName, SYMBOL_FIELD, ACCESS_NONE, DEFINITION_NONE);

						m_client->recordReference(
							REFERENCE_TEMPLATE_MEMBER_SPECIALIZATION,
							referencedName,
							getAstVisitor()->getDeclNameCache()->getValue(d),
							getParseLocation(d->getLocation())
						);
						break;
					}
				}
			}
		}
	}
}

void CxxAstVisitorComponentIndexer::visitFunctionDecl(clang::FunctionDecl* d)
{
	if (getAstVisitor()->shouldVisitDecl(d))
	{
		if (d->isFirstDecl())
		{
			m_client->recordSymbolWithLocationAndScopeAndSignature(
				getAstVisitor()->getDeclNameCache()->getValue(d),
				clang::isa<clang::CXXMethodDecl>(d) ? SYMBOL_METHOD : SYMBOL_FUNCTION,
				getParseLocation(d->getNameInfo().getSourceRange()),
				getParseLocationOfFunctionBody(d),
				getSignatureLocation(d),
				utility::convertAccessSpecifier(d->getAccess()),
				utility::isImplicit(d) ? DEFINITION_IMPLICIT : DEFINITION_EXPLICIT
			);
		}
		else
		{
			m_client->recordSymbolWithLocationAndScope(
				getAstVisitor()->getDeclNameCache()->getValue(d),
				clang::isa<clang::CXXMethodDecl>(d) ? SYMBOL_METHOD : SYMBOL_FUNCTION,
				getParseLocation(d->getNameInfo().getSourceRange()),
				getParseLocationOfFunctionBody(d),
				utility::convertAccessSpecifier(d->getAccess()),
				utility::isImplicit(d) ? DEFINITION_IMPLICIT : DEFINITION_EXPLICIT
			);
		}

		if (d->isFunctionTemplateSpecialization())
		{
			const NameHierarchy referencedName = getAstVisitor()->getDeclNameCache()->getValue(d->getPrimaryTemplate()->getTemplatedDecl()); // todo: use context and childcontext!!

			m_client->recordSymbol(referencedName, SYMBOL_FUNCTION, ACCESS_NONE, DEFINITION_NONE);

			m_client->recordReference(
				REFERENCE_TEMPLATE_SPECIALIZATION,
				referencedName,
				getAstVisitor()->getDeclNameCache()->getValue(d),
				getParseLocation(d->getLocation())
			);
		}
	}
}

void CxxAstVisitorComponentIndexer::visitCXXMethodDecl(clang::CXXMethodDecl* d)
{
	// Decl has been recorded in VisitFunctionDecl
	if (getAstVisitor()->shouldVisitDecl(d))
	{
		for (clang::CXXMethodDecl::method_iterator it = d->begin_overridden_methods(); // TODO: iterate in traversal and use REFERENCE_OVERRIDE or so..
			it != d->end_overridden_methods(); it++)
		{
			const NameHierarchy referencedName = getAstVisitor()->getDeclNameCache()->getValue(*it);

			m_client->recordSymbol(referencedName, SYMBOL_FUNCTION, ACCESS_NONE, DEFINITION_NONE);

			m_client->recordReference(
				REFERENCE_OVERRIDE,
				referencedName,
				getAstVisitor()->getDeclNameCache()->getValue(d),
				getParseLocation(d->getLocation())
			);
		}

		recordTemplateMemberSpecialization(
			d->getMemberSpecializationInfo(),
			getAstVisitor()->getDeclNameCache()->getValue(d),
			getParseLocation(d->getLocation()),
			SYMBOL_FUNCTION
		);
	}
}

void CxxAstVisitorComponentIndexer::visitEnumConstantDecl(clang::EnumConstantDecl* d)
{
	if (getAstVisitor()->shouldVisitDecl(d))
	{
		m_client->recordSymbolWithLocation(
			getAstVisitor()->getDeclNameCache()->getValue(d),
			SYMBOL_ENUM_CONSTANT,
			getParseLocation(d->getLocation()),
			ACCESS_NONE,
			utility::isImplicit(d) ? DEFINITION_IMPLICIT : DEFINITION_EXPLICIT
		);
	}
}

void CxxAstVisitorComponentIndexer::visitNamespaceDecl(clang::NamespaceDecl* d)
{
	if (getAstVisitor()->shouldVisitDecl(d))
	{
		m_client->recordSymbolWithLocationAndScope(
			getAstVisitor()->getDeclNameCache()->getValue(d),
			SYMBOL_NAMESPACE,
			getParseLocation(d->getLocation()),
			getParseLocation(d->getSourceRange()),
			utility::convertAccessSpecifier(d->getAccess()),
			utility::isImplicit(d) ? DEFINITION_IMPLICIT : DEFINITION_EXPLICIT
		);
	}
}

void CxxAstVisitorComponentIndexer::visitNamespaceAliasDecl(clang::NamespaceAliasDecl* d)
{
	if (getAstVisitor()->shouldVisitDecl(d))
	{
		m_client->recordSymbolWithLocation(
			getAstVisitor()->getDeclNameCache()->getValue(d),
			SYMBOL_NAMESPACE,
			getParseLocation(d->getLocation()),
			utility::convertAccessSpecifier(d->getAccess()),
			utility::isImplicit(d) ? DEFINITION_IMPLICIT : DEFINITION_EXPLICIT
		);

		m_client->recordReference(
			REFERENCE_USAGE,
			getAstVisitor()->getDeclNameCache()->getValue(d->getAliasedNamespace()),
			getAstVisitor()->getDeclNameCache()->getValue(d),
			getParseLocation(d->getTargetNameLoc())
		);

		// TODO: record other namespace as undefined
	}
}

void CxxAstVisitorComponentIndexer::visitTypedefDecl(clang::TypedefDecl* d)
{
	if (getAstVisitor()->shouldVisitDecl(d))
	{
		m_client->recordSymbolWithLocation(
			getAstVisitor()->getDeclNameCache()->getValue(d),
			d->getAnonDeclWithTypedefName() == nullptr ? SYMBOL_TYPEDEF : utility::convertTagKind(d->getAnonDeclWithTypedefName()->getTagKind()),
			getParseLocation(d->getLocation()),
			utility::convertAccessSpecifier(d->getAccess()),
			utility::isImplicit(d) ? DEFINITION_IMPLICIT : DEFINITION_EXPLICIT
		);
	}
}

void CxxAstVisitorComponentIndexer::visitTypeAliasDecl(clang::TypeAliasDecl* d)
{
	if (getAstVisitor()->shouldVisitDecl(d))
	{
		m_client->recordSymbolWithLocation(
			getAstVisitor()->getDeclNameCache()->getValue(d),
			d->getAnonDeclWithTypedefName() == nullptr ? SYMBOL_TYPEDEF : utility::convertTagKind(d->getAnonDeclWithTypedefName()->getTagKind()),
			getParseLocation(d->getLocation()),
			utility::convertAccessSpecifier(d->getAccess()),
			utility::isImplicit(d) ? DEFINITION_IMPLICIT : DEFINITION_EXPLICIT
		);
	}
}

void CxxAstVisitorComponentIndexer::visitUsingDirectiveDecl(clang::UsingDirectiveDecl* d)
{
	if (getAstVisitor()->shouldVisitDecl(d))
	{
		const NameHierarchy nameHierarchy = getAstVisitor()->getDeclNameCache()->getValue(d->getNominatedNamespaceAsWritten());

		m_client->recordSymbol(nameHierarchy, SYMBOL_NAMESPACE, ACCESS_NONE, DEFINITION_NONE);

		ParseLocation loc = getParseLocation(d->getLocation());
		m_client->recordReference(
			REFERENCE_USAGE,
			nameHierarchy,
			getAstVisitor()->getComponent<CxxAstVisitorComponentContext>()->getContextName(NameHierarchy(loc.filePath.wstr(), NAME_DELIMITER_FILE)),
			loc
		);
	}
}

void CxxAstVisitorComponentIndexer::visitUsingDecl(clang::UsingDecl* d)
{
	if (getAstVisitor()->shouldVisitDecl(d))
	{
		ParseLocation loc = getParseLocation(d->getLocation());
		m_client->recordReference(
			REFERENCE_USAGE,
			getAstVisitor()->getDeclNameCache()->getValue(d),
			getAstVisitor()->getComponent<CxxAstVisitorComponentContext>()->getContextName(NameHierarchy(loc.filePath.wstr(), NAME_DELIMITER_FILE)),
			loc
		);
	}
}

void CxxAstVisitorComponentIndexer::visitNonTypeTemplateParmDecl(clang::NonTypeTemplateParmDecl* d)
{
	if (getAstVisitor()->shouldVisitDecl(d) && !d->getName().empty()) // We don't create symbols for unnamed template parameters.
	{
		m_client->recordSymbolWithLocation(
			getAstVisitor()->getDeclNameCache()->getValue(d),
			SYMBOL_TEMPLATE_PARAMETER,
			getParseLocation(d->getLocation()),
			ACCESS_TEMPLATE_PARAMETER,
			utility::isImplicit(d) ? DEFINITION_IMPLICIT : DEFINITION_EXPLICIT
		);
	}
}

void CxxAstVisitorComponentIndexer::visitTemplateTypeParmDecl(clang::TemplateTypeParmDecl* d)
{
	if (getAstVisitor()->shouldVisitDecl(d) && !d->getName().empty()) // We don't create symbols for unnamed template parameters.
	{
		m_client->recordSymbolWithLocation(
			getAstVisitor()->getDeclNameCache()->getValue(d),
			SYMBOL_TEMPLATE_PARAMETER,
			getParseLocation(d->getLocation()),
			ACCESS_TEMPLATE_PARAMETER,
			utility::isImplicit(d) ? DEFINITION_IMPLICIT : DEFINITION_EXPLICIT
		);
	}
}

void CxxAstVisitorComponentIndexer::visitTemplateTemplateParmDecl(clang::TemplateTemplateParmDecl* d)
{
	if (getAstVisitor()->shouldVisitDecl(d) && !d->getName().empty()) // We don't create symbols for unnamed template parameters.
	{
		m_client->recordSymbolWithLocation(
			getAstVisitor()->getDeclNameCache()->getValue(d),
			SYMBOL_TEMPLATE_PARAMETER,
			getParseLocation(d->getLocation()),
			ACCESS_TEMPLATE_PARAMETER,
			utility::isImplicit(d) ? DEFINITION_IMPLICIT : DEFINITION_EXPLICIT
		);
	}
}

void CxxAstVisitorComponentIndexer::visitTypeLoc(clang::TypeLoc tl)
{
	if ((getAstVisitor()->shouldVisitReference(tl.getBeginLoc(), getAstVisitor()->getComponent<CxxAstVisitorComponentContext>()->getTopmostContextDecl())) &&
		(!getAstVisitor()->checkIgnoresTypeLoc(tl)))
	{
		if (clang::dyn_cast_or_null<clang::BuiltinType>(tl.getTypePtr()))
		{
			m_client->recordSymbol(getAstVisitor()->getTypeNameCache()->getValue(tl.getTypePtr()), SYMBOL_BUILTIN_TYPE, ACCESS_NONE, DEFINITION_EXPLICIT);
		}

		clang::SourceLocation loc;
		if (!tl.getAs<clang::DependentNameTypeLoc>().isNull())
		{
			const clang::DependentNameTypeLoc& dntl = tl.castAs<clang::DependentNameTypeLoc>();
			loc = dntl.getNameLoc();
		}
		else
		{
			loc = tl.getBeginLoc();
		}

		m_client->recordReference(
			getAstVisitor()->getComponent<CxxAstVisitorComponentTypeRefKind>()->getReferenceKind(),
			getAstVisitor()->getTypeNameCache()->getValue(tl.getTypePtr()),
			getAstVisitor()->getComponent<CxxAstVisitorComponentContext>()->getContextName(1), // we skip the last element because it refers to this typeloc.
			getParseLocation(loc)
		);
	}
}

void CxxAstVisitorComponentIndexer::visitDeclRefExpr(clang::DeclRefExpr* s)
{
	clang::ValueDecl* decl = s->getDecl();
	if (getAstVisitor()->shouldVisitReference(s->getLocation(), getAstVisitor()->getComponent<CxxAstVisitorComponentContext>()->getTopmostContextDecl()))
	{
		if ((clang::isa<clang::ParmVarDecl>(decl)) ||
			(clang::isa<clang::VarDecl>(decl) && decl->getParentFunctionOrMethod() != nullptr)
			) {
			ParseLocation declLocation = getParseLocation(decl->getLocation());
			std::wstring name = declLocation.filePath.fileName() + L"<" +
				std::to_wstring(declLocation.startLineNumber) + L":" +
				std::to_wstring(declLocation.startColumnNumber) + L">";

			m_client->recordLocalSymbol(name, getParseLocation(s->getLocation()));
		}
		else
		{
			const ReferenceKind refKind = consumeDeclRefContextKind();
			const NameHierarchy referencedName = getAstVisitor()->getDeclNameCache()->getValue(s->getDecl());

			if (refKind == REFERENCE_CALL)
			{
				m_client->recordSymbol(referencedName, SYMBOL_FUNCTION, ACCESS_NONE, DEFINITION_NONE);
			}

			m_client->recordReference(
				refKind,
				referencedName,
				getAstVisitor()->getComponent<CxxAstVisitorComponentContext>()->getContextName(),
				getParseLocation(s->getLocation())
			);
		}
	}
}

void CxxAstVisitorComponentIndexer::visitMemberExpr(clang::MemberExpr* s)
{
	if (getAstVisitor()->shouldVisitReference(s->getMemberLoc(), getAstVisitor()->getComponent<CxxAstVisitorComponentContext>()->getTopmostContextDecl()))
	{
		const ReferenceKind refKind = consumeDeclRefContextKind();
		const NameHierarchy referencedName = getAstVisitor()->getDeclNameCache()->getValue(s->getMemberDecl());

		if (refKind == REFERENCE_CALL)
		{
			m_client->recordSymbol(referencedName, SYMBOL_FUNCTION, ACCESS_NONE, DEFINITION_NONE);
		}

		m_client->recordReference(
			refKind,
			referencedName,
			getAstVisitor()->getComponent<CxxAstVisitorComponentContext>()->getContextName(),
			getParseLocation(s->getMemberLoc())
		);
	}
}

void CxxAstVisitorComponentIndexer::visitCXXConstructExpr(clang::CXXConstructExpr* s)
{
	const clang::CXXConstructorDecl* constructorDecl = s->getConstructor();

	if (!constructorDecl)
	{
		return;
	}
	else
	{
		const clang::CXXRecordDecl* parentDecl = constructorDecl->getParent();
		if (!parentDecl || parentDecl->isLambda())
		{
			return;
		}
	}

	if (getAstVisitor()->shouldVisitReference(s->getLocation(), getAstVisitor()->getComponent<CxxAstVisitorComponentContext>()->getTopmostContextDecl()))
	{
		//if (e->getParenOrBraceRange().isValid()) {
		//    // XXX: This code is a kludge.  Recording calls to constructors is
		//    // troublesome because there isn't an obvious location to associate the
		//    // call with.  Consider:
		//    //     A::A() : field(1, 2, 3) {}
		//    //     new A<B>(1, 2, 3)
		//    //     struct A { A(B); }; A f() { B b; return b; }
		//    // Implicit calls to conversion operator methods pose a similar
		//    // problem.
		//    //
		//    // Recording constructor calls is very useful, though, so, as a
		//    // temporary measure, when there are constructor arguments surrounded
		//    // by parentheses, associate the call with the right parenthesis.
		//    //
		//    // Perhaps the right fix is to associate the call with the line itself
		//    // or with a larger span which may have other references nested within
		//    // it.  The fix may have implications for the navigator GUI.
		//    RecordDeclRefExpr(
		//                e->getConstructor(),
		//                e->getParenOrBraceRange().getEnd(),
		//                e,
		//                CF_Called);
		//}
		clang::SourceLocation loc;
		clang::SourceLocation braceBeginLoc = s->getParenOrBraceRange().getBegin();
		clang::SourceLocation nameBeginLoc = s->getSourceRange().getBegin();
		if (braceBeginLoc.isValid())
		{
			if (braceBeginLoc == nameBeginLoc)
			{
				loc = nameBeginLoc;
			}
			else
			{
				loc = braceBeginLoc.getLocWithOffset(-1);
			}
		}
		else
		{
			loc = s->getSourceRange().getEnd();
		}
		loc = clang::Lexer::GetBeginningOfToken(loc, m_astContext->getSourceManager(), m_astContext->getLangOpts());

		const ReferenceKind refKind = consumeDeclRefContextKind();
		const NameHierarchy referencedName = getAstVisitor()->getDeclNameCache()->getValue(s->getConstructor());

		if (refKind == REFERENCE_CALL)
		{
			m_client->recordSymbol(referencedName, SYMBOL_FUNCTION, ACCESS_NONE, DEFINITION_NONE);
		}

		m_client->recordReference(
			refKind,
			referencedName,
			getAstVisitor()->getComponent<CxxAstVisitorComponentContext>()->getContextName(),
			getParseLocation(loc)
		);
	}
}

void CxxAstVisitorComponentIndexer::visitLambdaExpr(clang::LambdaExpr* s)
{
	clang::CXXMethodDecl* methodDecl = s->getCallOperator();
	if (getAstVisitor()->shouldVisitDecl(methodDecl))
	{
		m_client->recordSymbolWithLocationAndScope(
			getAstVisitor()->getDeclNameCache()->getValue(methodDecl),
			SYMBOL_FUNCTION,
			getParseLocation(s->getLocStart()),
			getParseLocationOfFunctionBody(methodDecl),
			ACCESS_NONE,  // TODO: introduce AccessLambda
			utility::isImplicit(methodDecl) ? DEFINITION_IMPLICIT : DEFINITION_EXPLICIT
		);
	}
}

void CxxAstVisitorComponentIndexer::visitConstructorInitializer(clang::CXXCtorInitializer* init)
{
	if (getAstVisitor()->shouldVisitReference(init->getMemberLocation(), getAstVisitor()->getComponent<CxxAstVisitorComponentContext>()->getTopmostContextDecl()))
	{
		// record the field usage here because it is not a DeclRefExpr
		if (clang::FieldDecl* memberDecl = init->getMember())
		{
			m_client->recordReference(
				REFERENCE_USAGE,
				getAstVisitor()->getDeclNameCache()->getValue(memberDecl),
				getAstVisitor()->getComponent<CxxAstVisitorComponentContext>()->getContextName(),
				getParseLocation(init->getMemberLocation())
			);
		}
	}
}

void CxxAstVisitorComponentIndexer::recordTemplateMemberSpecialization(
	const clang::MemberSpecializationInfo* memberSpecializationInfo, const NameHierarchy& context, const ParseLocation& location, SymbolKind symbolKind)
{
	if (memberSpecializationInfo != nullptr)
	{
		clang::NamedDecl* specializedNamedDecl = memberSpecializationInfo->getInstantiatedFrom();
		const NameHierarchy referencedName = getAstVisitor()->getDeclNameCache()->getValue(specializedNamedDecl);

		m_client->recordSymbol(referencedName, symbolKind, ACCESS_NONE, DEFINITION_NONE);

		m_client->recordReference(
			REFERENCE_TEMPLATE_MEMBER_SPECIALIZATION,
			referencedName,
			context,
			location
		);
	}
}

ParseLocation CxxAstVisitorComponentIndexer::getSignatureLocation(clang::FunctionDecl* d)
{
	clang::SourceRange signatureRange = d->getSourceRange();

	if (d->doesThisDeclarationHaveABody())
	{
		const clang::TypeSourceInfo *TSI = d->getTypeSourceInfo();
		if (!TSI)
		{
			return ParseLocation();
		}

		clang::FunctionTypeLoc FTL = TSI->getTypeLoc().IgnoreParens().getAs<clang::FunctionTypeLoc>();
		if (FTL.isNull())
		{
			return ParseLocation();
		}

		const clang::SourceManager& sm = m_astContext->getSourceManager();
		const clang::LangOptions& opts = m_astContext->getLangOpts();

		clang::SourceLocation endLoc = FTL.getSourceRange().getEnd();
		while (endLoc < signatureRange.getEnd())
		{
			llvm::Optional<clang::Token> token = clang::Lexer::findNextToken(endLoc, sm, opts);
			if (token.hasValue())
			{
				const clang::tok::TokenKind tokenKind = token.getValue().getKind();
				if (tokenKind == clang::tok::l_brace || tokenKind == clang::tok::colon)
				{
					signatureRange.setEnd(endLoc);
					break;
				}
				endLoc = token.getValue().getLocation();
			}
			else
			{
				return ParseLocation();
			}
		}
	}

	return getParseLocation(signatureRange);
}

ParseLocation CxxAstVisitorComponentIndexer::getParseLocationOfTagDeclBody(clang::TagDecl* decl) const
{
	return getAstVisitor()->getParseLocationOfTagDeclBody(decl);
}

ParseLocation CxxAstVisitorComponentIndexer::getParseLocationOfFunctionBody(const clang::FunctionDecl* decl) const
{
	return getAstVisitor()->getParseLocationOfFunctionBody(decl);
}

ParseLocation CxxAstVisitorComponentIndexer::getParseLocation(const clang::SourceLocation& loc) const
{
	return getAstVisitor()->getParseLocation(loc);
}

ParseLocation CxxAstVisitorComponentIndexer::getParseLocation(const clang::SourceRange& sourceRange) const
{
	return getAstVisitor()->getParseLocation(sourceRange);
}

ReferenceKind CxxAstVisitorComponentIndexer::consumeDeclRefContextKind()
{
	ReferenceKind refKind = REFERENCE_UNDEFINED;

	std::shared_ptr<CxxAstVisitorComponentTypeRefKind> typeRefKindComponent = getAstVisitor()->getComponent<CxxAstVisitorComponentTypeRefKind>();

	if (typeRefKindComponent->getReferenceKind() == REFERENCE_TYPE_USAGE)
	{
		refKind = getAstVisitor()->getComponent<CxxAstVisitorComponentDeclRefKind>()->getReferenceKind();
	}
	else
	{
		refKind = typeRefKindComponent->getReferenceKind();
	}
	return refKind;
}
