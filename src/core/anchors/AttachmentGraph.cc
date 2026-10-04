#include "core/anchors/AttachmentGraph.h"
#include <algorithm>
#include <functional>
#include <sstream>
namespace Anchors {
GraphResult attachmentGraph(const std::map<std::string,Semantic::Instance>& instances,const std::map<ReferenceKey,Attachment>& attachments) {
  GraphResult out;std::map<std::string,std::set<std::string>> edges;edges[""];
  auto fail=[&](const std::string& text,const Attachment* relation=nullptr) {
    out.diagnostics.push_back({"attachment-graph",text,relation && relation->declaration ? relation->declaration->source : std::nullopt});
  };
  for(const auto& [path,instance]:instances) {
    edges[path];
    if(!instance.parentPath.empty() && !instances.count(instance.parentPath))fail("Missing containment parent for " + path);
    edges[instance.parentPath].insert(path);
  }
  for(const auto& [key,relation]:attachments) {
    const auto& child=relation.child.path;const auto& target=relation.target.path;
    if(child.empty() || !instances.count(child) || (!target.empty() && !instances.count(target))) {fail("Missing attachment endpoint for " + key.id,&relation);continue;}
    auto [driver,inserted]=out.drivers.emplace(child,key);
    if(!inserted){fail("Multiple placement drivers for " + child,&relation);fail("Multiple placement drivers for " + child,&attachments.at(driver->second));}
    if(child==target){fail("Self attachment: " + child,&relation);continue;}
    if(target.rfind(child+"/",0)==0){fail("A descendant " + target + " cannot drive ancestor " + child,&relation);continue;}
    edges[target].insert(child);
  }
  if(!out.valid())return out;
  std::map<std::string,size_t> indegree;
  for(const auto& [path,children]:edges){indegree[path];for(const auto& child:children)++indegree[child];}
  std::set<std::string> ready;for(const auto& [path,count]:indegree)if(count==0)ready.insert(path);
  while(!ready.empty()) {
    auto path=*ready.begin();ready.erase(ready.begin());out.order.push_back(path);
    for(const auto& child:edges.at(path))if(--indegree[child]==0)ready.insert(child);
  }
  if(out.order.size()==edges.size())return out;
  std::map<std::string,int> color;std::vector<std::string> stack,cycle;
  std::function<bool(const std::string&)> visit=[&](const std::string& path) {
    color[path]=1;stack.push_back(path);
    for(const auto& child:edges.at(path)) {
      if(color[child]==1){cycle.assign(std::find(stack.begin(),stack.end(),child),stack.end());cycle.push_back(child);return true;}
      if(color[child]==0 && visit(child))return true;
    }
    stack.pop_back();color[path]=2;return false;
  };
  for(const auto& [path,children]:edges)if(color[path]==0 && visit(path))break;
  std::ostringstream message;message<<"Attachment cycle";for(const auto& path:cycle)message<<" -> "<<(path.empty()?"[document]":path);
  std::set<std::string> involved(cycle.begin(),cycle.end());
  for(const auto& [key,relation]:attachments)if(involved.count(relation.child.path) && involved.count(relation.target.path))fail(message.str()+"; relation " + key.id,&relation);
  if(out.diagnostics.empty())fail(message.str());out.order.clear();return out;
}
}
