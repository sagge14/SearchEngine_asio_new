#include "AccessBundle.hpp"
#include "TokenDocument.hpp"
#include "TokenLoader.hpp"
#include "CryptoStub.hpp"
#include "AuthorityRecovery.hpp"
#include "Auth/AuthClientStore.h"
#include "Auth/IdentitySigning.h"
#include "Auth/RsaIdentitySignatureVerifier.h"
#include "Commands/Auth/AuthenticateCmd.h"
#include <Windows.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace fs = std::filesystem;
using access_setup::Json;
namespace {
int checks = 0;
void Check(bool value, const char* label) { if (!value) throw std::runtime_error(label); ++checks; std::cout << "PASS " << label << '\n'; }
template<class F> void Reject(F f, const char* label) { bool rejected = false; try { f(); } catch (const std::exception&) { rejected = true; } Check(rejected, label); }
std::string Bytes(const fs::path& p) { std::ifstream s(p, std::ios::binary); return {std::istreambuf_iterator<char>(s), {}}; }
token_issuer::TokenFields Identity(const char* id, const char* uuid) { token_issuer::TokenFields f; f.client_id=id;f.client_name=id;f.device_type="computer";f.device_id=uuid;return f; }
Json Token(const token_issuer::TokenFields& f, const std::string& private_key) {
    return token_issuer::BuildTokenDocument(f, token_issuer::SignTokenPayload(
        auth::BuildIdentitySigningMessage(f.client_id,f.client_name,f.device_type,f.device_id), private_key));
}
Json Request(const token_issuer::TokenFields& f, const std::string& role) {
    return {{"format","searchengine-access-request"},{"version",1},
        {"request_id","B68B2F17-3333-4444-8888-112233445566"},{"computer_uuid",f.device_id},{"role",role},
        {"identity",role=="server" ? Json(nullptr) : token_issuer::BuildComputerRequestDocument(f)}};
}
void Authenticate(auth::AuthClientStore& store,const fs::path& key,const Json& token) {
    auth::RsaIdentitySignatureVerifier verifier(key); AuthenticateCmd command(store,verifier);
    Json body={{"client_id",token["client_id"]},{"client_name",token["client_name"]},
        {"device_type",token["device_type"]},{"device_id",token["device_id"]},{"signature",token["signature"]["value"]}};
    const auto text=body.dump(); Check(!command.executeResult({text.begin(),text.end()}).failed(),"server authenticates the supplied signed client");
}
}
int main() {
    const auto root=fs::temp_directory_path()/(L"searchengine-access-test-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    fs::create_directories(root);
    try {
        const auto keys=token_issuer::ResolveKeystorePaths(root/"keys");
        token_issuer::GenerateKeyPair(keys,"disposable-access-test-password");
        const auto private_key=token_issuer::UnlockPrivateKey(keys,"disposable-access-test-password");
        const auto public_key=Bytes(keys.public_key);
        const auto a=Identity("PC-A","11111111-2222-3333-4444-555555555555");
        const auto b=Identity("PC-B","AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE");
        const auto c=Identity("PC-C","12345678-1234-1234-1234-123456789ABC");
        const auto ta=Token(a,private_key),tb=Token(b,private_key),tc=Token(c,private_key);
        auto sourceWithExtra=ta;sourceWithExtra["private_key"]="must not be exported";
        Json tokens=Json::array();access_setup::AddToken(tokens,sourceWithExtra);access_setup::AddToken(tokens,tb);
        Check(!tokens[0].contains("private_key"),"exports exclude unknown or secret-bearing local metadata");
        access_setup::AddToken(tokens,tb);Check(tokens.size()==2,"repeated token import is idempotent");
        const auto request=Request(b,"client_server");const auto package=access_setup::MakePackage(request,public_key,tokens);
        access_setup::ValidateReplyForComputer(package,request,b.device_id);Check(true,"reply matches its requesting computer");
        auto changed=package;changed["tokens"][0]["private_key"]="unexpected";
        Reject([&]{access_setup::ValidatePackage(changed);},"received token metadata cannot carry unexpected fields");
        changed=request;changed["request_id"]="C68B2F17-3333-4444-8888-112233445566";
        Reject([&]{access_setup::ValidateReplyForComputer(package,changed,b.device_id);},"reply to a stale request is rejected");
        Reject([&]{access_setup::ValidateReplyForComputer(package,request,a.device_id);},"reply cannot install another PC token");
        const auto serverOnly=access_setup::MakePackage(Request(b,"server"),public_key,tokens);
        Check(serverOnly["request"]["identity"].is_null(),"server-only enrollment needs no client token");
        access_setup::MakePackage(Request(b,"client"),public_key,Json::array({tb}));Check(true,"client-only enrollment accepts its single token");
        changed=package;changed["tokens"][0]["client_name"]="Tampered";
        Reject([&]{access_setup::ValidatePackage(changed);},"tampered identity signature is rejected");
        changed=package;changed["public_key"]=private_key;
        Reject([&]{access_setup::ValidatePackage(changed);},"private keys cannot be transported");
        changed=package;changed["tokens"].push_back(tb);
        Reject([&]{access_setup::ValidatePackage(changed);},"duplicate client IDs in a response are rejected");
        changed=package;changed["unexpected_path"]="C:/Windows";
        Reject([&]{access_setup::ValidatePackage(changed);},"unexpected response fields are rejected");
        changed=package;changed["tokens"]=Json::array({ta});
        Reject([&]{access_setup::ValidatePackage(changed);},"response missing the requested client is rejected");
        for(const auto* name:{"A","B"}){fs::create_directories(root/name);access_setup::InstallTrust(root/name,public_key,tokens);}
        for(const auto* name:{"A","B"}){auth::AuthClientStore store;store.open(root/name/"auth_clients.sqlite");
            Check(store.listClients().size()==2,"each server has both client permissions");
            Authenticate(store,root/name/"issuer-public.pem",ta);Authenticate(store,root/name/"issuer-public.pem",tb);}
        access_setup::AddToken(tokens,tc);access_setup::MakePackage(Request(c,"client"),public_key,tokens);
        for(const auto* name:{"A","B"})access_setup::InstallTrust(root/name,public_key,tokens);
        {auth::AuthClientStore store;store.open(root/"B"/"auth_clients.sqlite");Authenticate(store,root/"B"/"issuer-public.pem",tc);
         store.setEnabled(b.client_id,false);}
        access_setup::InstallTrust(root/"B",public_key,tokens);
        {auth::AuthClientStore store;store.open(root/"B"/"auth_clients.sqlite");Check(!store.getClient(b.client_id)->enabled,"repeated setup does not re-enable a disabled client");}
        auto conflict=b;conflict.client_id=a.client_id;conflict.client_name=a.client_name;
        Json bad=Json::array({Token(conflict,private_key)});
        Reject([&]{access_setup::InstallTrust(root/"A",public_key,bad);},"conflicting device cannot replace an existing client");
        {auth::AuthClientStore store;store.open(root/"A"/"auth_clients.sqlite");Check(store.getClient(a.client_id)->device_id==a.device_id,"conflict preserves the original device identity");}
        fs::create_directories(root/"failure"/"issuer-public.pem");
        Reject([&]{access_setup::InstallTrust(root/"failure",public_key,tokens);},"key publication failure stops installation");
        {auth::AuthClientStore store;store.open(root/"failure"/"auth_clients.sqlite");Check(store.listClients().empty(),"key publication failure rolls back every registration");}
        const auto unicodeData=root/L"данные сервера с пробелами";fs::create_directories(unicodeData);
        access_setup::InstallTrust(unicodeData,public_key,tokens);
        {auth::AuthClientStore store;store.open(unicodeData/"auth_clients.sqlite");
         Authenticate(store,unicodeData/"issuer-public.pem",ta);}
        Check(fs::is_regular_file(unicodeData/"auth_clients.sqlite"),"Unicode database paths use the intended directory");
        const auto packet=root/L"пакет с пробелами.json";access_setup::WriteDocument(packet,package);
        Check(access_setup::ReadDocument(packet)==package,"Unicode access package paths round trip");
        access_setup::WriteBytes(root/"huge.json",std::string(1024*1024+1,' '));
        Reject([&]{access_setup::ReadDocument(root/"huge.json");},"oversized access packages are rejected");
        const auto fingerprint=access_setup::PublicKeyFingerprint(public_key);
        Check(access_setup::AuthorityUsesPublicKey({{"public_key_fingerprint",fingerprint}},public_key),"matching authority key is accepted");
        Check(!access_setup::AuthorityUsesPublicKey({{"public_key_fingerprint",fingerprint+"00"}},public_key),"changed authority key is detected");
        auto usb=Identity("USB-1",a.device_id.c_str());usb.device_type="usb";usb.device_id="USB-HARDWARE-123";
        const auto oldUsb=Token(usb,private_key);
        const auto statePath=root/L"профиль администратора"/"access-authority.json";
        Json state={{"computer_uuid",a.device_id},{"role","client_server"},{"services",Json::array({"A","B"})},
            {"public_key_fingerprint",fingerprint},{"tokens",Json::array({ta,tb,oldUsb})}};
        access_setup::WriteDocument(statePath,state);
        const auto oldEncrypted=Bytes(keys.private_enc);
        auto badState=state;badState["tokens"].push_back(ta);
        Reject([&]{token_issuer::RecoverAuthority(keys,statePath,badState,"new-password");},"invalid recovery directory rejected before changing old keys");
        Check(Bytes(keys.private_enc)==oldEncrypted,"invalid preparation preserves active keys");
        const auto foreign=token_issuer::AuthorityRecoveryPending(keys);fs::create_directories(foreign);
        access_setup::WriteBytes(foreign/"unrelated.txt","do not delete");
        Reject([&]{token_issuer::RecoverAuthority(keys,statePath,state,"new-password");},"recovery refuses to delete an unrelated pending directory");
        Check(Bytes(foreign/"unrelated.txt")=="do not delete" && Bytes(keys.private_enc)==oldEncrypted,"unrelated files and original keys remain intact");
        fs::remove_all(foreign);
        token_issuer::RecoverAuthority(keys,statePath,state,"new-password");
        const auto next=access_setup::ReadDocument(statePath);const auto newPublic=Bytes(keys.public_key);
        Check(newPublic!=public_key && next["reissue_prepared"]==true,"recovery prepares a new key and keeps activation separate");
        Check(Bytes(root/"B"/"issuer-public.pem")==public_key,"preparation does not switch existing servers");
        Reject([&]{token_issuer::UnlockPrivateKey(keys,"disposable-access-test-password");},"old password no longer unlocks replaced keys");
        token_issuer::UnlockPrivateKey(keys,"new-password");Check(true,"new password unlocks replaced keys");
        for(const auto& t:next["tokens"]) Check(access_setup::VerifyToken(t,newPublic),"every recovered PC/USB token verifies with the new key");
        Check(!access_setup::VerifyToken(ta,newPublic),"old token signature is rejected by new public key");
        Check(next["tokens"][2]["client_id"]==oldUsb["client_id"] && next["tokens"][2]["device_id"]==oldUsb["device_id"],"USB identity and binding preserved");
        access_setup::InstallTrust(root/"B",newPublic,next["tokens"]);
        {auth::AuthClientStore store;store.open(root/"B"/"auth_clients.sqlite");
         Check(!store.getClient(b.client_id)->enabled,"key replacement preserves explicitly disabled permission");
         Check(store.getClient(c.client_id).has_value(),"key replacement retains unrelated permission rows");
         Authenticate(store,root/"B"/"issuer-public.pem",next["tokens"][0]);}
        Check(!fs::exists(root/"B"/"issuer-public.before-access.pem"),"server activation makes no old-public-key backup");
        Check(!fs::exists(token_issuer::AuthorityRecoveryPending(keys)),"completed recovery removes new-key staging");
        Check(!fs::exists(fs::path(keys.root.wstring()+L".before-reissue")),"recovery makes no old-keystore backup");
        // Simulate a power interruption between publication of new public/private files.
        const auto pending=token_issuer::AuthorityRecoveryPending(keys);
        const auto staged=token_issuer::ResolveKeystorePaths(pending/"keys");
        token_issuer::GenerateKeyPair(staged,"resumable-password");const auto stagedPrivate=token_issuer::UnlockPrivateKey(staged,"resumable-password");
        const auto stagedPublic=Bytes(staged.public_key);auto stagedState=next;
        for(auto& t:stagedState["tokens"])t=token_issuer::ResignToken(t,stagedPrivate);
        stagedState["public_key_fingerprint"]=access_setup::PublicKeyFingerprint(stagedPublic);
        access_setup::WriteDocument(pending/"state.json",stagedState);
        const auto targetText=fs::weakly_canonical(statePath).u8string();
        access_setup::WriteDocument(pending/"ready.json",{{"state_path",std::string(targetText.begin(),targetText.end())}});
        access_setup::WriteBytes(keys.public_key,stagedPublic);
        Reject([&]{token_issuer::RecoverAuthority(keys,statePath,Json{},"wrong-new-password");},"resume requires the prepared new key password");
        Check(fs::exists(pending/"ready.json"),"failed resume preserves only new-key staging for retry");
        Reject([&]{token_issuer::RecoverAuthority(keys,root/"wrong-state.json",Json{},"resumable-password");},"pending recovery cannot target another state file");
        token_issuer::RecoverAuthority(keys,statePath,Json{},"resumable-password");
        Check(Bytes(keys.public_key)==stagedPublic,"resume publishes the same staged key without generating another");
        const auto resumed=access_setup::ReadDocument(statePath);
        Check(access_setup::VerifyToken(resumed["tokens"][0],stagedPublic),"resumed catalogue agrees with completed key replacement");
        fs::remove_all(root);std::cout<<"Access setup: "<<checks<<" checks passed\n";return 0;
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';fs::remove_all(root);return 1;}
}
