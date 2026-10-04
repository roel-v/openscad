#include "core/design/DesignSourceFields.h"
#include "core/design/DesignKinds.h"
#include "core/str_utf8_wrapper.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <string_view>

namespace Design {
namespace {
struct Token { std::string text, decoded; size_t begin,end; enum Type { Name,String,Number,Punctuation } type; };
class Scanner {
public:
  std::string_view bytes;size_t position,end;
  Scanner(const std::string& bytes,size_t begin,size_t end):bytes(bytes),position(begin),end(end){}
  void trivia(){
    while(position<end){
      if(std::isspace(static_cast<unsigned char>(bytes[position]))){++position;continue;}
      if(bytes.substr(position,2)=="//"){position+=2;while(position<end && bytes[position]!='\n')++position;continue;}
      if(bytes.substr(position,2)=="/*"){auto close=bytes.find("*/",position+2);if(close==std::string_view::npos || close+2>end)throw std::invalid_argument("Unterminated comment");position=close+2;continue;}
      break;
    }
  }
  std::optional<Token> next(){
    trivia();if(position==end)return {};size_t begin=position;char ch=bytes[position++];
    if(ch=='"'){
      std::string decoded;bool closed=false;
      while(position<end){char c=bytes[position++];if(c=='"'){closed=true;break;}if(c=='\n')continue;
        if(c!='\\'){decoded+=c;continue;}if(position==end)throw std::invalid_argument("Truncated escape");c=bytes[position++];
        if(c=='n')decoded+='\n';else if(c=='t')decoded+='\t';else if(c=='r')decoded+='\r';else if(c=='\n')continue;
        else if(c=='\r' && position<end && bytes[position]=='\n'){++position;continue;}
        else if(c=='x' || c=='u' || c=='U'){
          size_t count=c=='x'?2:c=='u'?4:6;if(position+count>end)throw std::invalid_argument("Truncated Unicode escape");
          std::string hex(bytes.substr(position,count));char *tail=nullptr;auto code=std::strtoul(hex.c_str(),&tail,16);if(tail!=hex.c_str()+hex.size())throw std::invalid_argument("Invalid escape");position+=count;
          decoded+=str_utf8_wrapper(static_cast<unsigned long>(code)).toString();
        }else if(c=='"' || c=='\\')decoded+=c;else throw std::invalid_argument("Unsupported string escape");
      }
      if(!closed)throw std::invalid_argument("Unterminated string");
      return Token{std::string(bytes.substr(begin,position-begin)),decoded,begin,position,Token::String};
    }
    if(std::isalpha(static_cast<unsigned char>(ch)) || ch=='_' || ch=='$' || static_cast<unsigned char>(ch)>=128){
      while(position<end && (std::isalnum(static_cast<unsigned char>(bytes[position])) || bytes[position]=='_' || static_cast<unsigned char>(bytes[position])>=128))++position;
      auto text=std::string(bytes.substr(begin,position-begin));return Token{text,text,begin,position,Token::Name};
    }
    if(std::isdigit(static_cast<unsigned char>(ch)) || (ch=='.' && position<end && std::isdigit(static_cast<unsigned char>(bytes[position])))){
      while(position<end && (std::isdigit(static_cast<unsigned char>(bytes[position])) || bytes[position]=='.'))++position;
      if(position<end && (bytes[position]=='e' || bytes[position]=='E')){++position;if(position<end && (bytes[position]=='+' || bytes[position]=='-'))++position;while(position<end && std::isdigit(static_cast<unsigned char>(bytes[position])))++position;}
      auto text=std::string(bytes.substr(begin,position-begin));return Token{text,text,begin,position,Token::Number};
    }
    return Token{std::string(1,ch),std::string(1,ch),begin,position,Token::Punctuation};
  }
  Token require(const std::string& text){auto token=next();if(!token || token->text!=text)throw std::invalid_argument("Expected "+text);return *token;}
};
SourceRange expression(Scanner& scanner){
  scanner.trivia();size_t begin=scanner.position,end=begin;std::vector<char> stack;
  while(auto token=scanner.next()){
    auto text=token->text;
    if(stack.empty() && (text=="," || text=="}")){scanner.position=token->begin;break;}
    if(token->type==Token::Punctuation){
      char ch=text[0];if(ch=='[' || ch=='{' || ch=='(')stack.push_back(ch);
      else if(ch==']' || ch=='}' || ch==')'){
        if(stack.empty() || (stack.back()=='['?']':stack.back()=='{'?'}':')')!=ch)throw std::invalid_argument("Mismatched expression delimiter");stack.pop_back();
      }
    }
    end=token->end;
  }
  if(!stack.empty() || end==begin)throw std::invalid_argument("Incomplete object field");return {begin,end};
}
ObjectFields object(Scanner& scanner){
  auto open=scanner.require("{");ObjectFields fields;fields.range.begin=open.begin;
  while(true){
    auto key=scanner.next();if(!key)throw std::invalid_argument("Unterminated object");
    if(key->text=="}"){fields.close=key->begin;fields.range.end=key->end;break;}
    if(key->type!=Token::Name && key->type!=Token::String)throw std::invalid_argument("Computed/comprehension keys require source editing");
    scanner.require(":");auto range=expression(scanner);
    if(!fields.fields.emplace(key->decoded,range).second)throw std::invalid_argument("Duplicate object fields require source editing");
    auto delimiter=scanner.next();if(!delimiter)throw std::invalid_argument("Unterminated object");
    if(delimiter->text=="}"){fields.close=delimiter->begin;fields.range.end=delimiter->end;break;}
    if(delimiter->text!=",")throw std::invalid_argument("Expected object comma");
    scanner.trivia();fields.trailingComma=scanner.position<scanner.end && scanner.bytes[scanner.position]=='}';
  }
  auto text=scanner.bytes.substr(fields.range.begin,fields.range.end-fields.range.begin);fields.multiline=text.find('\n')!=std::string_view::npos;fields.eol=text.find("\r\n")!=std::string_view::npos?"\r\n":"\n";
  if(!fields.fields.empty()){
    size_t position=fields.fields.begin()->second.begin;size_t line=scanner.bytes.rfind('\n',position);size_t start=line==std::string_view::npos?0:line+1;
    while(start<position && (scanner.bytes[start]==' ' || scanner.bytes[start]=='\t'))fields.indent+=scanner.bytes[start++];
  }
  if(fields.indent.empty()){
    size_t line=scanner.bytes.rfind('\n',fields.range.begin);size_t start=line==std::string_view::npos?0:line+1;
    while(start<fields.range.begin && (scanner.bytes[start]==' ' || scanner.bytes[start]=='\t'))fields.indent+=scanner.bytes[start++];fields.indent+="  ";
  }
  return fields;
}
bool literal(const std::string& bytes,SourceRange range,LiteralKind kind){
  try{
    Scanner scanner(bytes,range.begin,range.end);auto number=[&](){auto token=scanner.next();if(token && (token->text=="-" || token->text=="+"))token=scanner.next();if(!token || token->type!=Token::Number)return false;char *tail=nullptr;double value=std::strtod(token->text.c_str(),&tail);return tail==token->text.c_str()+token->text.size() && std::isfinite(value);};
    if(kind==LiteralKind::String){auto token=scanner.next();return token && token->type==Token::String && !scanner.next();}
    if(kind==LiteralKind::Number)return number() && !scanner.next();
    scanner.require("[");int count=kind==LiteralKind::Vector3?3:4;for(int i=0;i<count;++i){if(i)scanner.require(",");if(!number())return false;}scanner.require("]");return !scanner.next();
  }catch(const std::invalid_argument&){return false;}
}
}
DesignSourceMap DesignSourceMap::scan(const DeclarationKey& declaration,const SourceInput& input,const std::string& expectedKind){
  DesignSourceMap map;map.bytes=input.utf8;map.declaration=declaration;
  if(declaration.host!=DeclarationHost::Assignment){map.problem="Unsupported declaration host";return map;}
  registerBuiltins();auto handler=kind(expectedKind);
  if(!handler || !handler->hosts.count(DeclarationHost::Assignment) || declaration.fileKey!=input.fileKey || declaration.fingerprint!=input.fingerprint || declaration.endByte>input.utf8.size() || declaration.beginByte>=declaration.endByte){map.problem="Source identity or kind does not match";return map;}
  try{
    Scanner scanner(map.bytes,declaration.beginByte,declaration.endByte);scanner.require("@");scanner.require("{");scanner.require("type");scanner.require(":");scanner.require(expectedKind);scanner.require("}");
    auto name=scanner.next();if(!name || name->type!=Token::Name || name->decoded!=declaration.hostName)throw std::invalid_argument("Assignment name does not match");scanner.require("=");
    size_t rhs=scanner.position;
    // Establish annotation ownership even for a source-only helper RHS.
    Scanner ownership(map.bytes,rhs,declaration.endByte);std::vector<char> stack;bool ended=false;
    while(auto token=ownership.next()){
      if(token->type!=Token::Punctuation)continue;char ch=token->text[0];
      if(ch=='(' || ch=='[' || ch=='{')stack.push_back(ch);
      else if(ch==')' || ch==']' || ch=='}'){if(stack.empty() || (stack.back()=='('?')':stack.back()=='['?']':'}')!=ch)throw std::invalid_argument("Mismatched declaration");stack.pop_back();}
      else if(ch==';' && stack.empty()){if(ownership.next())throw std::invalid_argument("Declaration contains unrelated source");ended=true;break;}
    }
    if(!ended)throw std::invalid_argument("Declaration terminator is missing");map.owned=true;
    map.root=object(scanner);scanner.require(";");if(scanner.next())throw std::invalid_argument("Unexpected declaration suffix");
    auto adjust=map.root.fields.find("adjust");if(adjust!=map.root.fields.end()){
      Scanner nested(map.bytes,adjust->second.begin,adjust->second.end);map.adjust=object(nested);if(nested.next())throw std::invalid_argument("Computed adjust requires source editing");
    }
  }catch(const std::invalid_argument& error){map.problem=std::string("Source-only editing: ")+error.what();}
  return map;
}
std::optional<SourcePatch> DesignSourceMap::deletion(std::string& error)const{
  if(!owned){error=problem.empty()?"Annotation ownership is unavailable":problem;return {};}
  return SourcePatch{{declaration.beginByte,declaration.endByte},bytes.substr(declaration.beginByte,declaration.endByte-declaration.beginByte),""};
}
std::optional<std::vector<SourcePatch>> DesignSourceMap::patches(const std::vector<FieldPatch>& changes,std::string& error,const std::set<std::string>& allowedFields)const{
  if(!direct()){error=problem;return {};}
  std::vector<SourcePatch> patches;std::map<std::string,FieldPatch> selected;
  for(const auto& change:changes)selected.insert_or_assign(change.path,change);
  std::map<std::string,std::string> rootInsert,adjustInsert;
  for(const auto& [path,change]:selected){
    bool nested=path.rfind("adjust.",0)==0;std::string name=nested?path.substr(7):path;
    if(!allowedFields.count(path) || name.empty() || name.find('.')!=std::string::npos){error="Unsupported design edit field";return {};}
    if(!literal(change.encoded,{0,change.encoded.size()},change.literal)){error="Replacement must be a finite typed literal";return {};}
    const ObjectFields *fields=nested?(adjust?&*adjust:nullptr):&root;
    auto found=fields?fields->fields.find(name):root.fields.end();
    if(fields && found!=fields->fields.end()){
      if(!literal(bytes,found->second,change.literal)){error="Source-only editing: "+path+" is an expression";return {};}
      patches.push_back({found->second,bytes.substr(found->second.begin,found->second.end-found->second.begin),change.encoded});
    }else(nested?adjustInsert:rootInsert).insert_or_assign(name,change.encoded);
  }
  auto insertion=[&](const ObjectFields& fields,const std::map<std::string,std::string>& values){
    if(values.empty())return;
    std::string text;size_t position=fields.close;
    if(fields.multiline){
      // Put a needed comma immediately after the previous expression, before any trailing comment.
      size_t comma=0;if(!fields.fields.empty() && !fields.trailingComma)for(const auto& [name,range]:fields.fields)comma=std::max(comma,range.end);
      auto line=bytes.rfind('\n',fields.close);if(line!=std::string::npos){size_t start=line+1;bool whitespace=true;for(size_t i=start;i<fields.close;++i)if(bytes[i]!=' ' && bytes[i]!='\t')whitespace=false;if(whitespace)position=start;}
      if(position==fields.close)text+=fields.eol;
      size_t i=0;for(const auto& [name,value]:values){text+=fields.indent+name+": "+value;if(++i<values.size() || fields.trailingComma)text+=",";text+=fields.eol;}
      if(comma){if(comma==position)text=","+text;else patches.push_back({{comma,comma},"",","});}
    }else{
      if(!fields.fields.empty() && !fields.trailingComma)text+=",";
      size_t i=0;for(const auto& [name,value]:values){if(i++ || !fields.fields.empty())text+=" ";text+=name+": "+value;if(i<values.size() || fields.trailingComma)text+=",";}
    }
    patches.push_back({{position,position},"",text});
  };
  if(!adjustInsert.empty()){
    if(adjust)insertion(*adjust,adjustInsert);
    else{std::string encoded="{";size_t i=0;for(const auto& [name,value]:adjustInsert){if(i++)encoded+=", ";encoded+=name+": "+value;}encoded+="}";rootInsert.emplace("adjust",std::move(encoded));}
  }
  insertion(root,rootInsert);
  std::sort(patches.begin(),patches.end(),[](const auto& a,const auto& b){return a.range.begin>b.range.begin;});
  // An insertion can share the last field's replacement end; half-open ranges remain disjoint.
  return patches;
}
}
