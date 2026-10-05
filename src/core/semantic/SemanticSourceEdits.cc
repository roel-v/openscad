#include "core/semantic/SemanticSourceEdits.h"
#include "core/EvaluationInputs.h"
#include "core/SourceFile.h"
#include "core/ModuleInstantiation.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string_view>
namespace Semantic {
namespace {
struct Token { size_t begin,end; std::string text; enum Kind{Word,Number,String,Punctuation} kind; };
// A bounded trivia/token reader for already parsed source ranges; it never
// substitutes for native parsing or discovers declarations by text matching.
std::vector<Token> tokens(std::string_view bytes,size_t begin,size_t end) {
  if(begin>end || end>bytes.size())throw std::invalid_argument("Source range is unavailable");
  std::vector<Token> result;
  for(size_t p=begin;p<end;){
    const unsigned char c=bytes[p];
    if(std::isspace(c)){++p;continue;}
    if(bytes.substr(p,2)=="//"){while(p<end && bytes[p]!='\n')++p;continue;}
    if(bytes.substr(p,2)=="/*"){const auto close=bytes.find("*/",p+2);if(close==std::string_view::npos || close+2>end)throw std::invalid_argument("Incomplete comment");p=close+2;continue;}
    const size_t start=p++;Token::Kind kind=Token::Punctuation;
    if(c=='"'){
      kind=Token::String;bool closed=false;
      while(p<end){const auto ch=bytes[p++];if(ch=='"'){closed=true;break;}if(ch=='\\' && p<end)++p;}
      if(!closed)throw std::invalid_argument("Incomplete string");
    }else if(std::isdigit(c) || (c=='.' && p<end && std::isdigit(static_cast<unsigned char>(bytes[p])))){
      kind=Token::Number;while(p<end && (std::isdigit(static_cast<unsigned char>(bytes[p])) || bytes[p]=='.'))++p;
      if(p<end && (bytes[p]=='e' || bytes[p]=='E')){++p;if(p<end && (bytes[p]=='+' || bytes[p]=='-'))++p;while(p<end && std::isdigit(static_cast<unsigned char>(bytes[p])))++p;}
    }else if(std::isalpha(c) || c=='_' || c=='$' || c>=128){
      kind=Token::Word;while(p<end && (std::isalnum(static_cast<unsigned char>(bytes[p])) || bytes[p]=='_' || static_cast<unsigned char>(bytes[p])>=128))++p;
    }
    result.push_back({start,p,std::string(bytes.substr(start,p-start)),kind});
  }
  return result;
}
bool literal(const std::string& text,Value::Type type) {
  auto list=tokens(text,0,text.size());size_t at=0;
  auto number=[&]{if(at<list.size() && (list[at].text=="+" || list[at].text=="-"))++at;return at<list.size() && list[at++].kind==Token::Number;};
  if(type==Value::Type::BOOL)return list.size()==1 && (list[0].text=="true" || list[0].text=="false");
  if(type==Value::Type::STRING)return list.size()==1 && list[0].kind==Token::String;
  if(type==Value::Type::NUMBER)return number() && at==list.size();
  if(type!=Value::Type::VECTOR || list.empty() || list[at++].text!="[")return false;
  if(at<list.size() && list[at].text=="]")return ++at==list.size();
  while(number()){
    if(at<list.size() && list[at].text=="]")return ++at==list.size();
    if(at==list.size() || list[at++].text!=",")return false;
    if(at<list.size() && list[at].text=="]")return ++at==list.size();
  }
  return false;
}
bool containsComment(const std::string& text) {
  const auto list=tokens(text,0,text.size());size_t end=0;
  for(const auto& token:list){const auto gap=text.substr(end,token.begin-end);if(gap.find('/')!=std::string::npos)return true;end=token.end;}
  return text.substr(end).find('/')!=std::string::npos;
}
std::pair<size_t,size_t> parentheses(const Scene& scene,const SourceProvenance::Site& site) {
  if(!site.head || !scene.syntax->original(*site.head))throw std::invalid_argument("Complete native argument-list span is unavailable");
  const auto& bytes=scene.inputs->consumed().at(site.head->fileKey).utf8;
  const auto list=tokens(bytes,site.head->begin,site.head->end);std::vector<size_t> stack;
  std::optional<std::pair<size_t,size_t>> pair;
  for(const auto& token:list)if(token.kind==Token::Punctuation){
    if(token.text=="(")stack.push_back(token.begin);
    else if(token.text==")"){if(stack.empty())throw std::invalid_argument("Argument delimiters do not match");const auto open=stack.back();stack.pop_back();if(stack.empty())pair={{open,token.begin}};}
  }
  if(!stack.empty() || !pair || list.empty() || list.back().text!=")")throw std::invalid_argument("Complete argument delimiters are unavailable");
  return *pair;
}
void patch(ParameterEdit& edit,const SourceProvenance::Span& owner,const std::string& bytes,size_t begin,size_t end,std::string replacement) {
  auto span=owner;span.begin=begin;span.end=end;edit.patches.push_back({span,bytes.substr(begin,end-begin),std::move(replacement)});
}
const SourceProvenance::Argument *argument(const ParameterEdit& edit,const Parameter& parameter) {
  if(!edit.owner)return nullptr;
  if(edit.target.role==TargetRole::Instance){
    if(parameter.fromDefault)return nullptr;
    if(!parameter.winningArgument || *parameter.winningArgument>=edit.owner->arguments.size())return nullptr;
    return &edit.owner->arguments[*parameter.winningArgument];
  }
  for(const auto& value:edit.owner->arguments)if(value.name && *value.name==parameter.name)return &value;
  return nullptr;
}
}
std::optional<std::string> encodeLiteral(const Value& value) {
  if(value.type()==Value::Type::BOOL)return value.toBool()?"true":"false";
  if(value.type()==Value::Type::NUMBER){
    const auto number=value.toDouble();if(!std::isfinite(number))return {};
    std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(std::numeric_limits<double>::max_digits10)<<number;return out.str();
  }
  if(value.type()==Value::Type::STRING){
    std::string text="\"";const char *hex="0123456789abcdef";
    for(unsigned char c:value.toString()){
      if(c=='"' || c=='\\'){text+='\\';text+=c;}else if(c=='\n')text+="\\n";else if(c=='\r')text+="\\r";else if(c=='\t')text+="\\t";
      else if(c<32 || c==127){text+="\\x";text+=hex[c>>4];text+=hex[c&15];}else text+=c;
    }
    return text+'"';
  }
  if(value.type()==Value::Type::VECTOR){
    std::string text="[";bool first=true;
    for(const auto& item:value.toVector()){if(item.type()!=Value::Type::NUMBER)return {};const auto encoded=encodeLiteral(item);if(!encoded)return {};if(!first)text+=", ";first=false;text+=*encoded;}
    return text+"]";
  }
  return {};
}
bool literalParameterSource(const Scene& scene,const Parameter& parameter) {
  if(!scene.syntax || !parameter.source || !parameter.source->hasExpression)return false;
  try{const auto original=scene.syntax->original(parameter.source->expression);return original && literal(*original,parameter.type);}catch(const std::exception&){return false;}
}
ParameterEdit initializeStructure(const Scene& scene,const std::string& fileKey,const std::string& label) {
  ParameterEdit edit;
  auto refuse=[&](std::string reason){edit.reason=std::move(reason);edit.patches.clear();return edit;};
  if(scene.marked || !scene.valid())return refuse("This model already has structure declarations or declaration errors.");
  if(!scene.inputs || !scene.syntax || scene.sourceFiles.empty() || scene.inputs->root().fileKey!=fileKey)
    return refuse("Preview this document before adding model structure.");
  if(label.empty())return refuse("Enter a part name.");
  const auto& input=scene.inputs->root();const auto& bytes=input.utf8;
  try {
    // Even an otherwise unchanged module gains an ancestor after wrapping. Do not
    // silently alter code that observes the caller stack, including library code.
    for(const auto& [path,source]:scene.inputs->consumed())for(const auto& token:tokens(source.utf8,0,source.utf8.size()))
      if(token.kind==Token::Word && (token.text=="$parent_modules" || token.text=="parent_module" || token.text=="$children"))
        return refuse("This model or a library inspects module-call context ($children or the parent stack). Add structure annotations manually to preserve that behavior.");
    std::vector<SourceProvenance::Span> calls;
    for(const auto& call:scene.sourceFiles.front()->scope->moduleInstantiations){
      const auto *site=scene.syntax->site(*call);
      if(!site || !site->wholeComplete || site->whole.fileKey!=fileKey || !scene.syntax->original(site->whole))
        return refuse("Some top-level geometry comes from an include or has no complete local source range. Annotate that geometry manually.");
      calls.push_back(site->whole);
    }
    if(calls.empty())return refuse("There is no top-level geometry to wrap. Add a geometry call and preview first.");
    std::sort(calls.begin(),calls.end(),[](const auto& a,const auto& b){return a.begin<b.begin;});
    const auto start=calls.front().begin;
    // Top-level braces are flattened by the parser; inserting inside one would
    // create a local definition. Require a genuinely file-scope insertion point.
    int depth=0;
    for(const auto& token:tokens(bytes,0,start))if(token.kind==Token::Punctuation){if(token.text=="{")++depth;else if(token.text=="}")--depth;}
    if(depth!=0)return refuse("Geometry begins inside a top-level block. Move it to file scope before adding structure.");
    size_t end=start;
    for(const auto& span:calls){
      if(span.begin<end)return refuse("Top-level geometry ranges overlap; annotate this model manually.");
      for(const auto& token:tokens(bytes,end,span.begin))if(token.text!=";")
        return refuse("Imports or declarations are interleaved with geometry. Move them before the first geometry call, then preview again.");
      end=span.end;
    }
    for(const auto& token:tokens(bytes,end,bytes.size()))if(token.text!=";")
      return refuse("Declarations or other source follow the geometry. Move them before the first geometry call, then preview again.");
    // Root/display modifiers inside a new Part do not necessarily retain their
    // former presentation meaning. Check parsed calls, not comment/string text.
    for(const auto& [key,site]:scene.syntax->sites())if(site.kind==SourceProvenance::SiteKind::ModuleCall){
      const auto text=scene.syntax->original(site.whole);if(!text)continue;
      const auto list=tokens(*text,0,text->size());
      if(!list.empty() && (list.front().text=="!" || list.front().text=="%" || list.front().text=="#"))
        return refuse("Root, background or highlight modifiers need manual structure annotations to preserve their display behavior.");
    }
    std::string name="scadmaxx_model";unsigned serial=1;
    auto exists=[&]{for(const auto& [path,source]:scene.inputs->consumed())if(source.utf8.find(name)!=std::string::npos)return true;return false;};
    while(exists())name="scadmaxx_model_"+std::to_string(serial++);
    const auto encoded=encodeLiteral(Value(label));if(!encoded)return refuse("The part name cannot be encoded.");
    const std::string newline=bytes.find("\r\n")!=std::string::npos?"\r\n":"\n";
    const auto prefix=newline+"@{type: Part, id: \""+name+"\", label: "+*encoded+"}"+newline+"module "+name+"() {"+newline;
    const auto suffix=newline+"}"+newline+newline+"@{type: Component, id: \""+name+"-instance\", label: "+*encoded+"}"+newline+name+"();"+newline;
    patch(edit,calls.front(),bytes,start,start,prefix);patch(edit,calls.back(),bytes,bytes.size(),bytes.size(),suffix);
    edit.editable=true;return edit;
  }catch(const std::exception& error){return refuse(error.what());}
}
ParameterEdit inspectParameter(const Scene& scene,TargetKey key,const Parameter& parameter) {
  ParameterEdit result;result.target=key;result.parameter=parameter.name;
  try{
    if(!scene.valid() || !scene.syntax || !scene.inputs)throw std::invalid_argument("Semantic source snapshot is unavailable");
    if(parameter.ambiguous)throw std::invalid_argument("Native argument binding is ambiguous");
    if(!parameter.value || !encodeLiteral(*parameter.value))throw std::invalid_argument("This evaluated value has no supported literal control");
    if(key.role==TargetRole::Instance){
      const auto found=scene.instances.find(key.id);if(found==scene.instances.end())throw std::invalid_argument("Instance no longer exists");
      const auto binding=found->second.parameters.find(parameter.name);
      if(binding==found->second.parameters.end() || binding->second.winningArgument!=parameter.winningArgument ||
          binding->second.fromDefault!=parameter.fromDefault || binding->second.type!=parameter.type ||
          binding->second.ambiguous!=parameter.ambiguous || binding->second.displayedValue!=parameter.displayedValue)
        throw std::invalid_argument("Parameter does not match the captured native binding");
      result.owner=found->second.call;
      if(!result.owner)throw std::invalid_argument("Native call span is unavailable");
      const auto calls=scene.callInstances.find(result.owner->syntax);
      if(calls==scene.callInstances.end() || calls->second.size()!=1)throw std::invalid_argument("This call produces multiple instances; edit its source or definition defaults");
      if(!parameter.fromDefault && !parameter.winningArgument)throw std::invalid_argument("Winning native argument is unavailable");
    }else{
      const auto found=scene.definitions.find(key.id);if(found==scene.definitions.end())throw std::invalid_argument("Definition no longer exists");
      result.owner=found->second.syntax;
    }
    if(!result.owner)throw std::invalid_argument("Native definition span is unavailable");
    (void)parentheses(scene,*result.owner);
    const auto *arg=argument(result,parameter);
    if(key.role==TargetRole::Definition && !arg)throw std::invalid_argument("Definition parameter span is unavailable");
    if(arg && arg->hasExpression){
      const auto original=scene.syntax->original(arg->expression);
      if(!original || !literal(*original,parameter.type))throw std::invalid_argument("The source value is an expression; edit source");
      if(containsComment(*original))throw std::invalid_argument("Literal contains comments; edit source to preserve them");
    }else if(key.role==TargetRole::Instance && !parameter.fromDefault)throw std::invalid_argument("Native argument expression is unavailable");
    result.editable=true;result.removable=key.role==TargetRole::Instance && arg && arg->name.has_value();
  }catch(const std::exception& error){result.reason=error.what();}
  return result;
}
ParameterEdit changeParameter(const Scene& scene,TargetKey key,const Parameter& parameter,const Value& value) {
  auto result=inspectParameter(scene,key,parameter);if(!result.editable)return result;
  try{
    const auto encoded=encodeLiteral(value);if(!encoded || value.type()!=parameter.type)throw std::invalid_argument("Replacement must have the supported parameter type");
    const auto *arg=argument(result,parameter);const auto& site=*result.owner;const auto& bytes=scene.inputs->consumed().at(site.whole.fileKey).utf8;
    if(arg && arg->hasExpression)patch(result,arg->expression,bytes,arg->expression.begin,arg->expression.end,*encoded);
    else if(key.role==TargetRole::Definition)patch(result,arg->whole,bytes,arg->whole.end,arg->whole.end," = "+*encoded);
    else{
      const auto [open,close]=parentheses(scene,site);const auto body=tokens(bytes,open+1,close);
      const bool trailing=!body.empty() && body.back().text==",";
      const auto eol=bytes.substr(open,close-open).find("\r\n")!=std::string::npos?"\r\n":"\n";
      const bool multiline=bytes.substr(open,close-open).find('\n')!=std::string::npos;
      size_t position=close;std::string text;
      if(multiline){
        const auto line=bytes.rfind('\n',close);size_t start=line==std::string::npos?close:line+1;
        const auto closing=bytes.substr(start,close-start);const bool closingLine=closing.find_first_not_of(" \t")==std::string::npos;
        std::string indent=closingLine?closing+"  ":"  ";
        if(!site.arguments.empty()){
          const auto first=site.arguments.front().whole.begin;const auto firstLine=bytes.rfind('\n',first);const auto firstStart=firstLine==std::string::npos?first:firstLine+1;
          const auto existing=bytes.substr(firstStart,first-firstStart);if(existing.find_first_not_of(" \t")==std::string::npos)indent=existing;
        }
        if(!site.arguments.empty() && !trailing){const auto end=site.arguments.back().whole.end;patch(result,site.whole,bytes,end,end,",");}
        if(closingLine)position=start;else text+=eol;
        text+=indent+parameter.name+" = "+*encoded+(trailing?",":"")+eol;
      }else{
        if(!body.empty() && !trailing)text+=",";
        if(!body.empty())text+=" ";text+=parameter.name+" = "+*encoded;if(trailing)text+=",";
      }
      patch(result,site.whole,bytes,position,position,std::move(text));
    }
  }catch(const std::exception& error){result.editable=false;result.reason=error.what();result.patches.clear();}
  return result;
}
ParameterEdit resetParameter(const Scene& scene,TargetKey key,const Parameter& parameter) {
  auto result=inspectParameter(scene,key,parameter);
  if(!result.editable || !result.removable){result.editable=false;if(result.reason.empty())result.reason="Only a named override can be removed safely";return result;}
  try{
    const auto& site=*result.owner;const auto& arg=*argument(result,parameter);const auto& bytes=scene.inputs->consumed().at(site.whole.fileKey).utf8;
    if(containsComment(bytes.substr(arg.whole.begin,arg.whole.end-arg.whole.begin)))throw std::invalid_argument("Override contains comments; edit source to preserve them");
    const auto [open,close]=parentheses(scene,site);
    const auto next=parameter.winningArgument.value()+1;const auto previous=parameter.winningArgument.value();
    const size_t following=next<site.arguments.size()?site.arguments[next].whole.begin:close;
    auto delimiter=tokens(bytes,arg.whole.end,following);
    if(delimiter.empty() && previous>0)delimiter=tokens(bytes,site.arguments[previous-1].whole.end,arg.whole.begin);
    if(delimiter.size()>1 || (!delimiter.empty() && delimiter.front().text!=","))throw std::invalid_argument("Override separator trivia is ambiguous");
    if(delimiter.empty() && site.arguments.size()>1)throw std::invalid_argument("Override separator is unavailable");
    patch(result,arg.whole,bytes,arg.whole.begin,arg.whole.end,"");
    if(!delimiter.empty())patch(result,site.whole,bytes,delimiter.front().begin,delimiter.front().end,"");
  }catch(const std::exception& error){result.editable=false;result.reason=error.what();result.patches.clear();}
  return result;
}
}
