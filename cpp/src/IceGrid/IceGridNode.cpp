// **********************************************************************
//
// Copyright (c) 2003-2009 ZeroC, Inc. All rights reserved.
//
// This copy of Ice is licensed to you under the terms described in the
// ICE_LICENSE file included in this distribution.
//
// **********************************************************************

#include <IceUtil/UUID.h>
#include <IceUtil/Timer.h>
#include <IceUtil/StringUtil.h>
#include <Ice/Ice.h>
#include <Ice/Locator.h>
#include <Ice/Service.h>
#include <IceGrid/Activator.h>
#include <IceGrid/NodeServerAdminRouter.h>
#include <IceGrid/RegistryI.h>
#include <IceGrid/FileUserAccountMapperI.h>
#include <IceGrid/NodeI.h>
#include <IceGrid/NodeSessionManager.h>
#include <IceGrid/TraceLevels.h>
#ifdef __BCPLUSPLUS__
#  include <IceGrid/ServerI.h>
#  include <IceGrid/AdminSessionI.h>
#  include <IceGrid/ReapThread.h>
#  include <IceGrid/Database.h>
#  include <IceGrid/WellKnownObjectsManager.h>
#endif
#include <IceGrid/DescriptorParser.h>
#include <IcePatch2/Util.h>

#ifdef _WIN32
#   include <direct.h>
#   include <sys/types.h>
#   include <sys/stat.h>
#   include <winsock2.h>
#   ifdef _MSC_VER
#      define S_ISDIR(mode) ((mode) & _S_IFDIR)
#      define S_ISREG(mode) ((mode) & _S_IFREG)
#   endif
#else
#   include <sys/stat.h>
#endif

using namespace std;
using namespace Ice;
using namespace IceGrid;

namespace
{

class ProcessI : public Process
{
public:
    
    ProcessI(const ActivatorPtr&, const ProcessPtr&);

    virtual void shutdown(const Current&);
    virtual void writeMessage(const std::string&, Int, const Current&);
    
private:
    
    ActivatorPtr _activator;
    ProcessPtr _origProcess;
};


class NodeService : public Service
{
public:

    NodeService();

    virtual bool shutdown();

protected:

    virtual bool start(int, char*[]);
    bool startImpl(int, char*[]);
    virtual void waitForShutdown();
    virtual bool stop();
    virtual CommunicatorPtr initializeCommunicator(int&, char*[], const InitializationData&);

private:

    void usage(const std::string&);

    ActivatorPtr _activator;
    IceUtil::TimerPtr _timer;
    RegistryIPtr _registry;
    NodeIPtr _node;
    NodeSessionManager _sessions;
    Ice::ObjectAdapterPtr _adapter;
};

class CollocatedRegistry : public RegistryI
{
public:

    CollocatedRegistry(const CommunicatorPtr&, const ActivatorPtr&, bool, bool);
    virtual void shutdown();

private:

    ActivatorPtr _activator;
};


class DefaultServantLocator : public Ice::ServantLocator
{
public:

    DefaultServantLocator(const ObjectPtr& servant) :
        _servant(servant)
    {
    }

    virtual ObjectPtr locate(const Current& c, LocalObjectPtr&)
    {
        return _servant;
    }

    virtual void finished(const Current&, const ObjectPtr&, const LocalObjectPtr&)
    {
    }

    virtual void deactivate(const string&)
    {
    }

private:
    ObjectPtr _servant;
};

} 


CollocatedRegistry::CollocatedRegistry(const CommunicatorPtr& com, 
                                       const ActivatorPtr& activator, 
                                       bool nowarn,
                                       bool readonly) :
    RegistryI(com, new TraceLevels(com, "IceGrid.Registry"), nowarn, readonly), 
    _activator(activator)
{
}

void
CollocatedRegistry::shutdown()
{
    _activator->shutdown();
}

ProcessI::ProcessI(const ActivatorPtr& activator, const ProcessPtr& origProcess) : 
    _activator(activator),
    _origProcess(origProcess)
{
}

void
ProcessI::shutdown(const Current&)
{
    _activator->shutdown();
}

void
ProcessI::writeMessage(const string& message, Int fd, const Current& current)
{
    _origProcess->writeMessage(message, fd, current);
}

NodeService::NodeService()
{
}

bool
NodeService::shutdown()
{
    assert(_activator);
    _activator->shutdown();
    _sessions.terminate(); // Unblock the main thread if it's blocked on waitForCreate()
    return true;
}

bool
NodeService::start(int argc, char* argv[])
{
    try
    {
        if(!startImpl(argc, argv))
        {
            stop();
            return false;
        }
    }
    catch(...)
    {
        stop();
        throw;
    }
    return true;
}

bool
NodeService::startImpl(int argc, char* argv[])
{
    bool nowarn = false;
    bool readonly = false;
    string desc;
    vector<string> targets;
    for(int i = 1; i < argc; ++i)
    {
        if(strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
        {
            usage(argv[0]);
            return false;
        }
        else if(strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0)
        {
            print(ICE_STRING_VERSION);
            return false;
        }
        else if(strcmp(argv[i], "--nowarn") == 0)
        {
            nowarn = true;
        }
        else if(strcmp(argv[i], "--readonly") == 0)
        {
            readonly = true;
        }
        else if(strcmp(argv[i], "--deploy") == 0)
        {
            if(i + 1 >= argc)
            {
                error("missing descriptor argument for option `" + string(argv[i]) + "'");
                usage(argv[0]);
                return false;
            }

            desc = argv[++i];

            while(i + 1 < argc && argv[++i][0] != '-')
            {
                targets.push_back(argv[i]);
            }
        }
        else
        {
            error("invalid option: `" + string(argv[i]) + "'");
            usage(argv[0]);
            return false;
        }
    }

    PropertiesPtr properties = communicator()->getProperties();

    //
    // Disable server idle time. Otherwise, the adapter would be
    // shutdown prematurely and the deactivation would fail.
    // Deactivation of the node relies on the object adapter
    // to be active since it needs to terminate servers.
    //
    // TODO: implement Ice.ServerIdleTime in the activator
    // termination listener instead?
    //
    properties->setProperty("Ice.ServerIdleTime", "0");

    //
    // Warn the user that setting Ice.ThreadPool.Server isn't useful.
    //
    if(!nowarn && properties->getPropertyAsIntWithDefault("Ice.ThreadPool.Server.Size", 0) > 0)
    {
        Warning out(communicator()->getLogger());
        out << "setting `Ice.ThreadPool.Server.Size' is not useful, ";
        out << "you should set individual adapter thread pools instead.";
    }
    
    int size = properties->getPropertyAsIntWithDefault("IceGrid.Node.ThreadPool.Size", 0);
    if(size <= 0)
    {
        properties->setProperty("IceGrid.Node.ThreadPool.Size", "1");
        size = 1;
    }

    int sizeMax = properties->getPropertyAsIntWithDefault("IceGrid.Node.ThreadPool.SizeMax", 0);
    if(sizeMax <= 0)
    {
        if(size >= sizeMax)
        {
            sizeMax = size * 10;
        }
        
        ostringstream os;
        os << sizeMax;
        properties->setProperty("IceGrid.Node.ThreadPool.SizeMax", os.str());
    }

    size = properties->getPropertyAsIntWithDefault("Ice.ThreadPool.Client.Size", 0);
    if(size <= 0)
    {
        properties->setProperty("Ice.ThreadPool.Client.Size", "1");
        size = 1;
    }
    sizeMax = properties->getPropertyAsIntWithDefault("Ice.ThreadPool.Client.SizeMax", 0);
    if(sizeMax <= 0)
    {
        if(size >= sizeMax)
        {
            sizeMax = size * 10;
        }
        if(sizeMax < 100)
        {
            sizeMax = 100;
        }
        ostringstream os;
        os << sizeMax;
        properties->setProperty("Ice.ThreadPool.Client.SizeMax", os.str());
    }

    //
    // Create the activator.
    //
    TraceLevelsPtr traceLevels = new TraceLevels(communicator(), "IceGrid.Node");
    _activator = new Activator(traceLevels);

    //
    // Collocate the IceGrid registry if we need to.
    //
    if(properties->getPropertyAsInt("IceGrid.Node.CollocateRegistry") > 0)
    {
        _registry = new CollocatedRegistry(communicator(), _activator, nowarn, readonly);
        if(!_registry->start())
        {
            return false;
        }

        //
        // Set the default locator property to point to the collocated
        // locator (this property is passed by the activator to each
        // activated server). The default locator is also needed by
        // the node session manager.
        //
        if(properties->getProperty("Ice.Default.Locator").empty())
        {
            Identity locatorId;
            locatorId.category = properties->getPropertyWithDefault("IceGrid.InstanceName", "IceGrid");
            locatorId.name = "Locator";
            string endpoints = properties->getProperty("IceGrid.Registry.Client.Endpoints");
            string locatorPrx = "\"" + communicator()->identityToString(locatorId) + "\" :" + endpoints;
            communicator()->setDefaultLocator(Ice::LocatorPrx::uncheckedCast(communicator()->stringToProxy(locatorPrx)));
            properties->setProperty("Ice.Default.Locator", locatorPrx);
        }
    }
    else if(properties->getProperty("Ice.Default.Locator").empty())
    {
        error("property `Ice.Default.Locator' is not set");
        return false;
    }

    //
    // Initialize the database environment (first setup the directory structure if needed).
    //
    string dataPath = properties->getProperty("IceGrid.Node.Data");
    string dbPath;
    if(dataPath.empty())
    {
        error("property `IceGrid.Node.Data' is not set");
        return false;
    }
    else
    {
#ifdef _WIN32
        struct _stat filestat;
        if(::_stat(dataPath.c_str(), &filestat) != 0 || !S_ISDIR(filestat.st_mode))
        {
            ostringstream os;
            FileException ex(__FILE__, __LINE__);
            ex.path = dataPath;
            ex.error = getSystemErrno();
            os << ex;
            error("property `IceGrid.Node.Data' is set to an invalid path:\n" + os.str());
            return false;
        }            
#else
        struct stat filestat;
        if(::stat(dataPath.c_str(), &filestat) != 0 || !S_ISDIR(filestat.st_mode))
        {
            ostringstream os;
            FileException ex(__FILE__, __LINE__);
            ex.path = dataPath;
            ex.error = getSystemErrno();
            os << ex;
            error("property `IceGrid.Node.Data' is set to an invalid path:\n" + os.str());
            return false;
        }            
#endif

        //
        // Creates subdirectories.
        //
        if(dataPath[dataPath.length() - 1] != '/')
        {
            dataPath += "/"; 
        }

        IcePatch2::createDirectory(dataPath + "servers");
        IcePatch2::createDirectory(dataPath + "tmp");
        IcePatch2::createDirectory(dataPath + "distrib");
    }

    //
    // Check that required properties are set and valid.
    //
    if(properties->getProperty("IceGrid.Node.Endpoints").empty())
    {
        error("property `IceGrid.Node.Endpoints' is not set");
        return false;
    }

    string name = properties->getProperty("IceGrid.Node.Name");
    if(name.empty())
    {
        error("property `IceGrid.Node.Name' is not set");
        return false;
    }

    //
    // Setup the Freeze database environment home directory. The name of the database
    // environment for the IceGrid node is the name of the node.
    //
    properties->setProperty("Freeze.DbEnv." + name + ".DbHome", dbPath);

    //
    // Create the node object adapter.
    //
    _adapter = communicator()->createObjectAdapter("IceGrid.Node");

    //
    // Setup the user account mapper if configured.
    //
    string mapperProperty = "IceGrid.Node.UserAccountMapper";
    string mapperPropertyValue = properties->getProperty(mapperProperty);
    UserAccountMapperPrx mapper;
    if(!mapperPropertyValue.empty())
    {
        try
        {
            mapper = UserAccountMapperPrx::uncheckedCast(communicator()->propertyToProxy(mapperProperty));
        }
        catch(const Ice::LocalException& ex)
        {
            ostringstream os;
            os << "user account mapper `" << mapperProperty << "' is invalid:\n" << ex;
            error(os.str());
            return false;
        }
    }
    else
    {
        string userAccountFileProperty = properties->getProperty("IceGrid.Node.UserAccounts");
        if(!userAccountFileProperty.empty())
        {
            try
            {
                Ice::ObjectPrx object = _adapter->addWithUUID(new FileUserAccountMapperI(userAccountFileProperty));
                object = object->ice_collocationOptimized(true);
                mapper = UserAccountMapperPrx::uncheckedCast(object);
            }
            catch(const std::string& msg)
            {
                error(msg);
                return false;
            }
        }
    }

    //
    // Create a new timer to handle server activation/deactivation timeouts.
    //
    _timer = new IceUtil::Timer();

    //
    // The IceGrid instance name.
    //
    const string instanceName = communicator()->getDefaultLocator()->ice_getIdentity().category;

    //
    // Create the server factory. The server factory creates persistent objects
    // for the server and server adapter. It also takes care of installing the
    // evictors and object factories necessary to store these objects.
    //
    Identity id = communicator()->stringToIdentity(instanceName + "/Node-" + name);
    NodePrx nodeProxy = NodePrx::uncheckedCast(_adapter->createProxy(id));
    _node = new NodeI(_adapter, _sessions, _activator, _timer, traceLevels, nodeProxy, name, mapper);
    _adapter->add(_node, nodeProxy->ice_getIdentity());

    _adapter->addServantLocator(new DefaultServantLocator(new NodeServerAdminRouter(_node)), _node->getServerAdminCategory());

    //
    // Start the platform info thread if needed.
    //
    _node->getPlatformInfo().start();

    //
    // Ensures that the locator is reachable.
    // 
    if(!nowarn)
    {
        try
        {
            communicator()->getDefaultLocator()->ice_timeout(15000)->ice_ping();
        }
        catch(const Ice::LocalException& ex)
        {
            Warning out(communicator()->getLogger());
            out << "couldn't reach the IceGrid registry (this is expected ";
            out << "if it's down, otherwise please check the value of the ";
            out << "Ice.Default.Locator property):\n" << ex;
        }
    }

    //
    // Create the node sessions with the registries.
    //
    _sessions.create(_node);

    //
    // In some tests, we deploy icegridnodes using IceGrid:
    //
    if(properties->getProperty("Ice.Admin.Endpoints") != "")
    {
        //
        // Replace Process facet and create Admin object
        //
        try
        {
            ProcessPtr origProcess = ProcessPtr::dynamicCast(communicator()->removeAdminFacet("Process"));
            communicator()->addAdminFacet(new ProcessI(_activator, origProcess), "Process");
            communicator()->getAdmin();
        }
        catch(const Ice::NotRegisteredException&)
        {
            //
            // Some plug-in removed the Process facet, so we don't replace it.
            // (unlikely error though)
            // 
        }
    }

    //
    // Start the activator.
    //
    _activator->start();

    //
    // Activate the adapter.
    //
    _adapter->activate();

    //
    // Notify the node session manager that the node can start
    // accepting incoming connections.
    //
    _sessions.activate();

    string bundleName = properties->getProperty("IceGrid.Node.PrintServersReady");
    if(!bundleName.empty() || !desc.empty())
    {
        enableInterrupt();
        if(!_sessions.waitForCreate())
        {
            //
            // Create was interrupted, return true as if the service was
            // correctly initiliazed to make sure it's properly stopped.
            //
            return true;
        }
        disableInterrupt();
    }

    //
    // Deploy application if a descriptor is passed as a command-line option.
    //
    if(!desc.empty())
    {
        try
        {
            Ice::Identity registryId;
            registryId.category = instanceName;
            registryId.name = "Registry";
            
            RegistryPrx registry = RegistryPrx::checkedCast(
                communicator()->stringToProxy("\"" + communicator()->identityToString(registryId) + "\""));
            if(!registry)
            {
                throw "invalid registry";
            }
        
            registry = registry->ice_preferSecure(true); // Use SSL if available.
            
            IceGrid::AdminSessionPrx session;
            if(communicator()->getProperties()->getPropertyAsInt("IceGridAdmin.AuthenticateUsingSSL"))
            {
                session = registry->createAdminSessionFromSecureConnection();
            }
            else
            {
                string id = communicator()->getProperties()->getProperty("IceGridAdmin.Username");
                string password = communicator()->getProperties()->getProperty("IceGridAdmin.Password");
                while(id.empty())
                {
                    cout << "user id: " << flush;
                    getline(cin, id);
                    id = IceUtilInternal::trim(id);
                }
                
                if(password.empty())
                {
                    cout << "password: " << flush;
                    getline(cin, password);
                    password = IceUtilInternal::trim(password);
                }
                
                session = registry->createAdminSession(id, password);
            }
            assert(session);

            AdminPrx admin = session->getAdmin();
            map<string, string> vars;
            ApplicationDescriptor app = DescriptorParser::parseDescriptor(desc, targets, vars, communicator(), admin);

            try
            {
                admin->syncApplication(app);
            }
            catch(const ApplicationNotExistException&)
            {
                admin->addApplication(app);
            }
        }
        catch(const DeploymentException& ex)
        {
            ostringstream ostr;
            ostr << "failed to deploy application `" << desc << "':\n" << ex << ": " << ex.reason;
            warning(ostr.str());
        }
        catch(const AccessDeniedException& ex)
        {
            ostringstream ostr;
            ostr << "failed to deploy application `" << desc << "':\n" 
                 << "registry database is locked by `" << ex.lockUserId << "'";
            warning(ostr.str());
        }
        catch(const std::exception& ex)
        {
            ostringstream ostr;
            ostr << "failed to deploy application `" << desc << "':\n" << ex.what();
            warning(ostr.str());
        }
        catch(const string& reason)
        {
            ostringstream ostr;
            ostr << "failed to deploy application `" << desc << "':\n" << reason;
            warning(ostr.str());
        }
    }

    if(!bundleName.empty())
    {
        print(bundleName + " ready");
    }

    return true;
}

void
NodeService::waitForShutdown()
{
    //
    // Wait for the activator shutdown. Once the run method returns
    // all the servers have been deactivated.
    //
    enableInterrupt();
    _activator->waitForShutdown();
    disableInterrupt();
}

bool
NodeService::stop()
{
    if(_activator)
    {
        try
        {
            _activator->shutdown();
            _activator->destroy();
        }
        catch(...)
        {
            assert(false);
        }
        _activator = 0;
    }

    if(_timer)
    {
        //
        // The timer must be destroyed after the activator and before the
        // communicator is shutdown.
        //
        try
        {
            _timer->destroy();
        }
        catch(...)
        {
            assert(false);
        }
        _timer = 0;
    }

    //
    // Deactivate the node object adapter.
    //
    if(_adapter)
    {
        try
        {
            _adapter->deactivate();
            _adapter = 0;
        }
        catch(const Ice::LocalException& ex)
        {
            ostringstream ostr;
            ostr << "unexpected exception while shutting down node:\n" << ex;
            warning(ostr.str());
        }
    }

    //
    // Terminate the node sessions with the registries.
    //
    _sessions.destroy();

    //
    // Stop the platform info thread.
    //
    if(_node)
    {
        _node->getPlatformInfo().stop();
    }

    //
    // We can now safely shutdown the communicator.
    //
    try
    {
        communicator()->shutdown();
        communicator()->waitForShutdown();
    }
    catch(const Ice::LocalException& ex)
    {
        ostringstream ostr;
        ostr << "unexpected exception while shutting down node:\n" << ex;
        warning(ostr.str());
    }

    //
    // Break cylic reference counts.
    //
    if(_node)
    {
        _node->shutdown();
        _node = 0;
    }

    //
    // And shutdown the collocated registry.
    //
    if(_registry)
    {
        _registry->stop();
        _registry = 0;
    }

    return true;
}

CommunicatorPtr
NodeService::initializeCommunicator(int& argc, char* argv[], 
                                    const InitializationData& initializationData)
{
    InitializationData initData = initializationData;
    initData.properties = createProperties(argc, argv, initData.properties);

    //
    // Make sure that IceGridNode doesn't use collocation optimization
    //
    initData.properties->setProperty("Ice.Default.CollocationOptimized", "0");

    //
    // Delay creation of Admin object:
    //
    initData.properties->setProperty("Ice.Admin.DelayCreation", "1");

    return Service::initializeCommunicator(argc, argv, initData);
}

void
NodeService::usage(const string& appName)
{
    string options =
        "Options:\n"
        "-h, --help           Show this message.\n"
        "-v, --version        Display the Ice version.\n"
        "--nowarn             Don't print any security warnings.\n"
        "--readonly           Start the collocated master registry in read-only mode."
        "\n"
        "--deploy DESCRIPTOR [TARGET1 [TARGET2 ...]]\n"
        "                     Add or update descriptor in file DESCRIPTOR, with\n"
        "                     optional targets.\n";
#ifndef _WIN32
    options.append(
        "\n"
        "\n"
        "--daemon             Run as a daemon.\n"
        "--noclose            Do not close open file descriptors.\n"
        "--nochdir            Do not change the current working directory.\n"
        "--pidfile FILE       Write process ID into FILE."
    );
#endif
    print("Usage: " + appName + " [options]\n" + options);
}

int
main(int argc, char* argv[])
{
    NodeService svc;
    return svc.main(argc, argv);
}