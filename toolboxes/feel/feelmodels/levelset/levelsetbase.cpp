#include <feel/feelmodels/levelset/levelsetbase.hpp>

#include <feel/feelmodels/modelmesh/createmesh.hpp>
#include <feel/feelmodels/levelset/reinitializer_hj.hpp>

#include <feel/feelmodels/levelset/cauchygreentensorexpr.hpp>
#include <feel/feelmodels/levelset/cauchygreeninvariantsexpr.hpp>
#include <feel/feelmodels/levelset/levelsetdeltaexpr.hpp>
#include <feel/feelmodels/levelset/levelsetheavisideexpr.hpp>

#include <boost/assign/list_of.hpp>
#include <boost/assign/list_inserter.hpp>

namespace Feel {
namespace FeelModels {

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
LEVELSETBASE_CLASS_TEMPLATE_TYPE::LevelSetBase( 
        std::string const& prefix,
        worldcomm_ptr_t const& worldComm,
        std::string const& subPrefix,
        ModelBaseRepository const& modelRep ) 
:
    super_type( prefix, worldComm, subPrefix, modelRep ),
    M_doUpdateDirac(true),
    M_doUpdateHeaviside(true),
    M_doUpdateInterfaceElements(true),
    M_doUpdateRangeDiracElements(true),
    M_doUpdateInterfaceFaces(true),
    M_doUpdateSmootherInterface(true),
    M_doUpdateSmootherInterfaceVectorial(true),
    M_doUpdateNormal(true),
    M_doUpdateCurvature(true),
    M_doUpdateGradPhi(true),
    M_doUpdateModGradPhi(true),
    M_doUpdatePhiPN(true),
    M_doUpdateDistance(true),
    M_doUpdateDistanceNormal(true),
    M_doUpdateDistanceCurvature(true),
    M_doUpdateSubmeshDirac(true),
    M_doUpdateSubmeshOuter(true),
    M_doUpdateSubmeshInner(true),
    M_doUpdateMarkers(true),
    M_useCurvatureDiffusion(false),
    //M_periodicity(periodicityLS),
    M_reinitializerIsUpdatedForUse(false),
    M_hasReinitialized(false)
{
    this->setFilenameSaveInfo( prefixvm(this->prefix(),"Levelset.info") );
    //-----------------------------------------------------------------------------//
    // Load parameters
    this->loadParametersFromOptionsVm();
    // Load initial value
    this->loadConfigICFile();
    // Load post-process
    this->loadConfigPostProcess();
    // Get periodicity from options (if needed)
    //this->loadPeriodicityFromOptionsVm();

    /*// --------------- mesh adaptation -----------------
#if defined (MESH_ADAPTATION)
    auto backend_mesh_adapt = backend_type::build(Environment::vm(), "mesh-adapt-backend");
    mesh_adapt.reset( new mesh_adaptation_type ( backend_mesh_adapt ));
#endif*/
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
typename LEVELSETBASE_CLASS_TEMPLATE_TYPE::self_ptrtype
LEVELSETBASE_CLASS_TEMPLATE_TYPE::New(
        std::string const& prefix,
        worldcomm_ptr_t const& worldComm,
        std::string const& subPrefix,
        ModelBaseRepository const& modelRep )
{
    self_ptrtype new_ls( new self_type(prefix, worldComm, subPrefix, modelRep ) );
    return new_ls;
}

//----------------------------------------------------------------------------//
LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
void
LEVELSETBASE_CLASS_TEMPLATE_TYPE::createMesh()
{
    this->log("LevelSetBase","createMesh","start");
    this->timerTool("Constructor").start();
    
    createMeshModel<mesh_type>(*this, M_mesh, this->fileNameMeshPath() );
    CHECK( M_mesh ) << "mesh generation failed";
    //M_isUpdatedForUse = false;

    double tElapsed = this->timerTool("Constructor").stop("create");
    this->log("LevelSetBase","createMesh", (boost::format("finish in %1% s") %tElapsed).str() );
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
void
LEVELSETBASE_CLASS_TEMPLATE_TYPE::init()
{
    this->log("LevelSet", "init", "start");

    // Mesh and space manager
    if( !M_spaceManager )
    {
        if( !M_mesh )
        {
            // Create mesh
            this->createMesh();
        }
        M_spaceManager = std::make_shared<levelset_space_manager_type>( M_mesh );
    }
    else
    {
        M_mesh = M_spaceManager->mesh();
    }
    // Tool manager
    if( !M_toolManager )
        M_toolManager = std::make_shared<levelset_tool_manager_type>( M_spaceManager, this->prefix() );
    // Function spaces
    this->createFunctionSpaces();
    // Tools
    this->createInterfaceQuantities();
    this->createReinitialization();
    this->createTools();
    this->createExporters();

    // Initial value
    if( !this->doRestart() )
    {
        // Set levelset initial value
        this->initLevelsetValue();
    }

    M_initialVolume = this->volume();
    M_initialPerimeter = this->perimeter();

    // Init post-process
    this->initPostProcess();

    this->log("LevelSet", "init", "finish");
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
void
LEVELSETBASE_CLASS_TEMPLATE_TYPE::initLevelsetValue()
{
    this->log("LevelSet", "initLevelsetValue", "start");

    if( !M_initialPhi ) // look for JSON initial values
    {
        bool hasInitialValue = false;

        auto phi_init = this->functionSpace()->elementPtr();
        phi_init->setConstant( std::numeric_limits<value_type>::max() );

        this->modelProperties().parameters().updateParameterValues();
        if( !this->M_icDirichlet.empty() )
        {
            M_icDirichlet.setParameterValues( this->modelProperties().parameters().toParameterValues() );

            for( auto const& iv : M_icDirichlet )
            {
                auto const& icMarkers = markers(iv);
                if( icMarkers.empty() )
                    continue;
                else
                {
                    for( std::string const& marker: icMarkers )
                    {
                        if( marker.empty() )
                        {
                            phi_init->on(
                                    _range=elements(phi_init->mesh()),
                                    _expr=expression(iv),
                                    _geomap=this->geomap()
                                    );
                        }
                        else
                        {
                            phi_init->on(
                                    _range=markedelements(phi_init->mesh(), marker),
                                    _expr=expression(iv),
                                    _geomap=this->geomap()
                                    );
                        }
                    }
                }
            }

            hasInitialValue = true;
        }

        if( !this->M_icShapes.empty() )
        {
            // If phi_init already has a value, ensure that it is a proper distance function
            if( hasInitialValue )
            {
                // ILP on phi_init
                auto const modGradPhiInit = this->modGrad( phi_init );
                    
                *phi_init = vf::project( 
                    _space=this->functionSpace(),
                    _range=elements(this->functionSpace()->mesh()),
                    _expr=idv(phi_init) / idv(modGradPhiInit)
                    );
                // Reinitialize phi_init
                *phi_init = this->reinitializerFM()->run( *phi_init );
            }
            // Add shapes
            for( auto const& shape: M_icShapes )
            {
                this->addShape( shape, *phi_init );
            }

            hasInitialValue = true;
        }

        if( hasInitialValue ) // has JSON-provided initial value
        {
            this->setInitialValue( phi_init );
        }
    }

    // Synchronize with current phi
    if( M_initialPhi ) // user-provided initial value
    {
        *M_phi = *M_initialPhi;
    }
    else // no initial value
    {
        M_phi->zero();
    }

    this->updateInterfaceQuantities();

    this->log("LevelSet", "initLevelsetValue", "finish");
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
void
LEVELSETBASE_CLASS_TEMPLATE_TYPE::addShape( 
        std::pair<ShapeType, parameter_map> const& shape, 
        element_levelset_type & phi 
        )
{
    ShapeType shapeType = shape.first;
    parameter_map const& shapeParams = shape.second;

    switch(shapeType)
    {
        case ShapeType::SPHERE:
        {
            auto X = Px() - shapeParams.dget("xc");
            auto Y = Py() - shapeParams.dget("yc");
            auto Z = Pz() - shapeParams.dget("zc"); 
            auto R = shapeParams.dget("radius");
            phi = vf::project(
                    _space=this->functionSpace(),
                    _range=elements(this->functionSpace()->mesh()),
                    _expr=vf::min( idv(phi), sqrt(X*X+Y*Y+Z*Z)-R ),
                    _geomap=this->geomap()
                    );
        }
        break;

        case ShapeType::ELLIPSE:
        {
            auto X = Px() - shapeParams.dget("xc");
            auto Y = Py() - shapeParams.dget("yc");
            auto Z = Pz() - shapeParams.dget("zc");
            double A = shapeParams.dget("a");
            double B = shapeParams.dget("b");
            double C = shapeParams.dget("c");
            double psi = shapeParams.dget("psi");
            double theta = shapeParams.dget("theta");
            // Apply inverse ZYX TaitBryan rotation
            double cosPsi = std::cos(psi); double sinPsi = std::sin(psi);
            double cosTheta = std::cos(theta); double sinTheta = std::sin(theta);
            auto Xp = cosTheta*(cosPsi*X+sinPsi*Y) + sinTheta*Z;
            auto Yp = -sinPsi*X + cosPsi*Y;
            auto Zp = -sinTheta*(cosPsi*X+sinPsi*Y) + cosTheta*Z;
            // Project
            phi = vf::project(
                    _space=this->functionSpace(),
                    _range=elements(this->functionSpace()->mesh()),
                    _expr=vf::min( idv(phi), sqrt(Xp*Xp+Yp*Yp*(A*A)/(B*B)+Zp*Zp*(A*A)/(C*C))-A ),
                    _geomap=this->geomap()
                    );
        }
        break;
    }
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
typename LEVELSETBASE_CLASS_TEMPLATE_TYPE::element_levelset_type
LEVELSETBASE_CLASS_TEMPLATE_TYPE::interfaceRectangularFunction( element_levelset_type const& p ) const
{
    auto phi = idv(p);
    double epsilon = M_thicknessInterfaceRectangularFunction;
    double epsilon_rect = 2.*epsilon;
    double epsilon_delta = (epsilon_rect - epsilon)/2.;
    double epsilon_zero = epsilon + epsilon_delta;

    auto R_expr =
        vf::chi( phi<-epsilon_rect )*vf::constant(0.0)
        +
        vf::chi( phi>=-epsilon_rect )*vf::chi( phi<=-epsilon )*
        0.5*(1 + (phi+epsilon_zero)/epsilon_delta + 1/M_PI*vf::sin( M_PI*(phi+epsilon_zero)/epsilon_delta ) )
        +
        vf::chi( phi>=-epsilon )*vf::chi( phi<=epsilon )*vf::constant(1.0)
        +
        vf::chi( phi>=epsilon )*vf::chi( phi<=epsilon_rect )*
        0.5*(1 - (phi-epsilon_zero)/epsilon_delta - 1/M_PI*vf::sin( M_PI*(phi-epsilon_zero)/epsilon_delta ) )
        +
        vf::chi(phi>epsilon_rect)*vf::constant(0.0)
        ;

    return vf::project( 
            this->functionSpace(), 
            this->rangeMeshElements(),
            R_expr
            );
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
void
LEVELSETBASE_CLASS_TEMPLATE_TYPE::initPostProcess()
{
    if (this->doRestart() && this->restartPath().empty() )
    {
        if ( M_exporter->doExport() ) M_exporter->restart(this->timeInitial());
    }

    this->modelProperties().parameters().updateParameterValues();
    auto paramValues = this->modelProperties().parameters().toParameterValues();
    this->modelProperties().postProcess().setParameterValues( paramValues );

    // Measures
    if ( !this->isStationary() )
    {
        if ( this->doRestart() )
            this->postProcessMeasuresIO().restart( "time", this->timeInitial() );
        else
            this->postProcessMeasuresIO().setMeasure( "time", this->timeInitial() ); //just for have time in first column
    }

    //// User-defined fields
    //if ( this->modelProperties().postProcess().find("Fields") != this->modelProperties().postProcess().end() )
    //{
        //for ( auto const& o : this->modelProperties().postProcess().find("Fields")->second )
        //{
            //if ( this->hasFieldUserScalar( o ) || this->hasFieldUserVectorial( o ) )
                //M_postProcessUserFieldExported.insert( o );
        //}
    //}
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
void
LEVELSETBASE_CLASS_TEMPLATE_TYPE::createFunctionSpaces()
{
    if( M_useSpaceIsoPN )
    {
        this->functionSpaceManager()->createFunctionSpaceIsoPN();
        M_spaceLevelset = this->functionSpaceManager()->functionSpaceScalarIsoPN();
        M_spaceVectorial = this->functionSpaceManager()->functionSpaceVectorialIsoPN();
        M_spaceMarkers = this->functionSpaceManager()->functionSpaceMarkersIsoPN();
    }
    else
    {
        this->functionSpaceManager()->createFunctionSpaceDefault();
        M_spaceLevelset = this->functionSpaceManager()->functionSpaceScalar();
        M_spaceVectorial = this->functionSpaceManager()->functionSpaceVectorial();
        M_spaceMarkers = this->functionSpaceManager()->functionSpaceMarkers();
    }

    M_rangeMeshElements = elements( M_spaceLevelset->mesh() );
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
void
LEVELSETBASE_CLASS_TEMPLATE_TYPE::createInterfaceQuantities()
{
    if( Environment::vm().count( prefixvm(this->prefix(),"thickness-interface").c_str() ) )
        M_thicknessInterface = doption(prefixvm(this->prefix(),"thickness-interface"));
    else
        M_thicknessInterface = 1.5 * this->mesh()->hAverage();

    M_useAdaptiveThicknessInterface = boption(prefixvm(this->prefix(),"use-adaptive-thickness"));

    if( Environment::vm().count( prefixvm(this->prefix(),"thickness-interface-rectangular-function").c_str() ) )
        M_thicknessInterfaceRectangularFunction = doption(prefixvm(this->prefix(),"thickness-interface-rectangular-function")); 
    else
        M_thicknessInterfaceRectangularFunction = M_thicknessInterface;

    M_phi.reset( new element_levelset_type(this->functionSpace(), "Phi") );
    M_heaviside.reset( new element_levelset_type(this->functionSpace(), "Heaviside") );
    M_dirac.reset( new element_levelset_type(this->functionSpace(), "Dirac") );
    M_levelsetNormal.reset( new element_vectorial_type(this->functionSpaceVectorial(), "Normal") );
    M_levelsetCurvature.reset( new element_levelset_type(this->functionSpace(), "Curvature") );
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
void
LEVELSETBASE_CLASS_TEMPLATE_TYPE::createReinitialization()
{
    switch( M_reinitMethod )
    { 
        case LevelSetDistanceMethod::NONE :
        {
            // Nothing to do
        }
        break;
        case LevelSetDistanceMethod::FASTMARCHING :
        {
            if( !M_reinitializer )
                M_reinitializer = this->reinitializerFM();

            std::dynamic_pointer_cast<reinitializerFM_type>( M_reinitializer )->setUseMarker2AsMarkerDone( 
                    M_useMarkerDiracAsMarkerDoneFM 
                    );
        }
        break;
        case LevelSetDistanceMethod::HAMILTONJACOBI :
        {
            if( !M_reinitializer )
                M_reinitializer = this->reinitializerHJ();
            
            double thickness_heaviside;
            if( Environment::vm( _name="thickness-heaviside", _prefix=prefixvm(this->prefix(), "reinit-hj")).defaulted() )
            {
                thickness_heaviside =  M_thicknessInterface;
            }
            else
            {
                thickness_heaviside =  doption( _name="thickness-heaviside", _prefix=prefixvm(this->prefix(), "reinit-hj") );
            }
            std::dynamic_pointer_cast<reinitializerHJ_type>(M_reinitializer)->setThicknessHeaviside( thickness_heaviside );
        }
        break;
        case LevelSetDistanceMethod::RENORMALISATION :
        // Nothing to do - Remove warning
        break;
    }

    M_reinitializerIsUpdatedForUse = true;
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
void
LEVELSETBASE_CLASS_TEMPLATE_TYPE::createTools()
{
    this->toolManager()->createProjectorL2Default();
    M_projectorL2Scalar = this->toolManager()->projectorL2Scalar();
    M_projectorL2Vectorial = this->toolManager()->projectorL2Vectorial();

    this->toolManager()->createProjectorSMDefault();
    M_projectorSMScalar = this->toolManager()->projectorSMScalar();
    M_projectorSMVectorial = this->toolManager()->projectorSMVectorial();

    if( this->useCurvatureDiffusion() )
    {
        this->toolManager()->createCurvatureDiffusion();
    }
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
void
LEVELSETBASE_CLASS_TEMPLATE_TYPE::createExporters()
{
    std::string geoExportType = "static";//this->geoExportType();//change_coords_only, change, static
    M_exporter = Feel::exporter( 
            _mesh=this->mesh(),
            _name="ExportLS",
            _geo=geoExportType,
            _path=this->exporterPath() 
            );
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
typename LEVELSETBASE_CLASS_TEMPLATE_TYPE::element_levelset_PN_ptrtype const&
LEVELSETBASE_CLASS_TEMPLATE_TYPE::phiPN() const
{
    CHECK( M_useSpaceIsoPN ) << "use-space-iso-pn must be enabled to use phiPN \n";

    if( !M_levelsetPhiPN )
        M_levelsetPhiPN.reset( new element_levelset_PN_type(this->functionSpaceManager()->functionSpaceScalarPN(), "PhiPN") );

    if( M_doUpdatePhiPN )
       const_cast<self_type*>(this)->updatePhiPN(); 

    return M_levelsetPhiPN;
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
typename LEVELSETBASE_CLASS_TEMPLATE_TYPE::element_vectorial_ptrtype const&
LEVELSETBASE_CLASS_TEMPLATE_TYPE::gradPhi() const
{
    if( !M_levelsetGradPhi )
        M_levelsetGradPhi.reset( new element_vectorial_type(this->functionSpaceVectorial(), "GradPhi") );

    if( M_doUpdateGradPhi )
       const_cast<self_type*>(this)->updateGradPhi(); 

    return M_levelsetGradPhi;
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
typename LEVELSETBASE_CLASS_TEMPLATE_TYPE::element_levelset_ptrtype const&
LEVELSETBASE_CLASS_TEMPLATE_TYPE::modGradPhi() const
{
    if( !M_levelsetModGradPhi )
        M_levelsetModGradPhi.reset( new element_levelset_type(this->functionSpace(), "ModGradPhi") );

    if( M_doUpdateModGradPhi )
        const_cast<self_type*>(this)->updateModGradPhi();

    return M_levelsetModGradPhi;
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
typename LEVELSETBASE_CLASS_TEMPLATE_TYPE::element_levelset_ptrtype const&
LEVELSETBASE_CLASS_TEMPLATE_TYPE::distance() const
{
    if( !M_distance )
        M_distance.reset( new element_levelset_type(this->functionSpace(), "Distance") );

    if( M_doUpdateDistance )
       const_cast<self_type*>(this)->updateDistance(); 

    return M_distance;
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
typename LEVELSETBASE_CLASS_TEMPLATE_TYPE::element_levelset_ptrtype const&
LEVELSETBASE_CLASS_TEMPLATE_TYPE::heaviside() const
{
    if( !M_heaviside )
        M_heaviside.reset( new element_levelset_type(this->functionSpace(), "Heaviside") );

    if( M_doUpdateHeaviside )
       const_cast<self_type*>(this)->updateHeaviside();

    return M_heaviside;
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
typename LEVELSETBASE_CLASS_TEMPLATE_TYPE::levelset_delta_expr_type
LEVELSETBASE_CLASS_TEMPLATE_TYPE::diracExpr() const
{
    return levelsetDelta(
            _element=this->phiElt(),
            _thickness=this->thicknessInterface(),
            _use_adaptive_thickness=this->M_useAdaptiveThicknessInterface,
            _use_local_redist=this->M_useRegularPhi,
            _use_distance_impl=false
            );
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
typename LEVELSETBASE_CLASS_TEMPLATE_TYPE::element_levelset_ptrtype const&
LEVELSETBASE_CLASS_TEMPLATE_TYPE::dirac() const
{
    if( !M_dirac )
        M_dirac.reset( new element_levelset_type(this->functionSpace(), "Dirac") );

    if( M_doUpdateDirac )
       const_cast<self_type*>(this)->updateDirac();

    return M_dirac;
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
typename LEVELSETBASE_CLASS_TEMPLATE_TYPE::element_vectorial_ptrtype const&
LEVELSETBASE_CLASS_TEMPLATE_TYPE::normal() const
{
    if( !M_levelsetNormal )
        M_levelsetNormal.reset( new element_vectorial_type(this->functionSpaceVectorial(), "Normal") );

    if( M_doUpdateNormal )
       const_cast<self_type*>(this)->updateNormal(); 

    return M_levelsetNormal;
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
typename LEVELSETBASE_CLASS_TEMPLATE_TYPE::element_levelset_ptrtype const&
LEVELSETBASE_CLASS_TEMPLATE_TYPE::curvature() const
{
    if( !M_levelsetCurvature )
        M_levelsetCurvature.reset( new element_levelset_type(this->functionSpace(), "Curvature") );

    if( M_doUpdateCurvature )
       const_cast<self_type*>(this)->updateCurvature();

    return M_levelsetCurvature;
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
typename LEVELSETBASE_CLASS_TEMPLATE_TYPE::element_vectorial_ptrtype const&
LEVELSETBASE_CLASS_TEMPLATE_TYPE::distanceNormal() const
{
    if( !M_distanceNormal )
        M_distanceNormal.reset( new element_vectorial_type(this->functionSpaceVectorial(), "DistanceNormal") );

    if( M_doUpdateDistanceNormal )
       const_cast<self_type*>(this)->updateDistanceNormal(); 

    return M_distanceNormal;
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
typename LEVELSETBASE_CLASS_TEMPLATE_TYPE::element_levelset_ptrtype const&
LEVELSETBASE_CLASS_TEMPLATE_TYPE::distanceCurvature() const
{
    if( !M_distanceCurvature )
        M_distanceCurvature.reset( new element_levelset_type(this->functionSpace(), "DistanceCurvature") );

    if( M_doUpdateDistanceCurvature )
       const_cast<self_type*>(this)->updateDistanceCurvature();

    return M_distanceCurvature;
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
void
LEVELSETBASE_CLASS_TEMPLATE_TYPE::setFastMarchingInitializationMethod( FastMarchingInitializationMethod m )
{
    if (M_reinitializerIsUpdatedForUse)
        LOG(INFO)<<" !!!  WARNING !!! : fastMarchingInitializationMethod set after the fast marching has been actually initialized ! \n";
    M_fastMarchingInitializationMethod = m;
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
void
LEVELSETBASE_CLASS_TEMPLATE_TYPE::loadParametersFromOptionsVm()
{
    M_useRegularPhi = boption(_name=prefixvm(this->prefix(),"use-regularized-phi"));
    M_useHeavisideDiracNodalProj = boption(_name=prefixvm(this->prefix(),"h-d-nodal-proj"));

    std::string reinitmethod = soption( _name="reinit-method", _prefix=this->prefix() );
    CHECK( LevelSetDistanceMethodIdMap.count( reinitmethod ) ) << reinitmethod << " is not in the list of possible redistantiation methods\n";
    M_reinitMethod = LevelSetDistanceMethodIdMap.at( reinitmethod );

    std::string distancemethod = soption( _name="distance-method", _prefix=this->prefix() );
    CHECK( LevelSetDistanceMethodIdMap.count( distancemethod ) ) << distancemethod << " is not in the list of possible redistantiation methods\n";
    M_distanceMethod = LevelSetDistanceMethodIdMap.at( distancemethod );

    M_useMarkerDiracAsMarkerDoneFM = boption( _name="use-marker2-as-done", _prefix=prefixvm(this->prefix(), "reinit-fm") );

    const std::string fm_init_method = soption( _name="fm-initialization-method", _prefix=this->prefix() );
    CHECK(FastMarchingInitializationMethodIdMap.left.count(fm_init_method)) << fm_init_method <<" is not in the list of possible fast-marching initialization methods\n";
    M_fastMarchingInitializationMethod = FastMarchingInitializationMethodIdMap.left.at(fm_init_method);

    M_reinitInitialValue = boption( _name="reinit-initial-value", _prefix=this->prefix() );

    const std::string gradPhiMethod = soption( _name="gradphi-method", _prefix=this->prefix() );
    CHECK(DerivationMethodMap.left.count(gradPhiMethod)) << gradPhiMethod <<" is not in the list of possible gradphi derivation methods\n";
    M_gradPhiMethod = DerivationMethodMap.left.at(gradPhiMethod);

    if( Environment::vm( _name="modgradphi-method", _prefix=this->prefix() ).defaulted() &&
        !Environment::vm( _name="gradphi-method", _prefix=this->prefix() ).defaulted() )
    {
        M_modGradPhiMethod = M_gradPhiMethod;
    }
    else
    {
        const std::string modGradPhiMethod = soption( _name="modgradphi-method", _prefix=this->prefix() );
        CHECK(DerivationMethodMap.left.count(modGradPhiMethod)) << modGradPhiMethod <<" is not in the list of possible modgradphi derivation methods\n";
        M_modGradPhiMethod = DerivationMethodMap.left.at(modGradPhiMethod);
    }

    const std::string curvatureMethod = soption( _name="curvature-method", _prefix=this->prefix() );
    CHECK(CurvatureMethodMap.left.count(curvatureMethod)) << curvatureMethod <<" is not in the list of possible curvature methods\n";
    M_curvatureMethod = CurvatureMethodMap.left.at(curvatureMethod);

    if( M_curvatureMethod == CurvatureMethod::DIFFUSION_ORDER1 
            || M_curvatureMethod == CurvatureMethod::DIFFUSION_ORDER2 )
        this->setUseCurvatureDiffusion( true );

    M_useSpaceIsoPN = boption( _name="use-space-iso-pn", _prefix=this->prefix() );
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
void
LEVELSETBASE_CLASS_TEMPLATE_TYPE::loadConfigICFile()
{
    auto const& initialConditions = this->modelProperties().initialConditions();

    this->M_icDirichlet = initialConditions.getScalarFields( std::string(this->prefix()), "Dirichlet" );
    
    // Shapes
    for( std::string const& icShape: initialConditions.markers( this->prefix(), "shapes") )
    {
        parameter_map shapeParameterMap;

        auto shapeTypeRead = initialConditions.sparam( this->prefix(), "shapes", icShape, "shape" );
        auto shapeTypeIt = ShapeTypeMap.find(shapeTypeRead.second);
        if( shapeTypeIt != ShapeTypeMap.end() )
        {
            switch(shapeTypeIt->second)
            {
                case ShapeType::SPHERE:
                {
                    auto xcRead = initialConditions.dparam( this->prefix(), "shapes", icShape, "xc" );
                    CHECK(xcRead.first) << icShape << " xc not provided\n";
                    auto ycRead = initialConditions.dparam( this->prefix(), "shapes", icShape, "yc" );
                    CHECK(ycRead.first) << icShape << " yc not provided\n";
                    auto zcRead = initialConditions.dparam( this->prefix(), "shapes", icShape, "zc" );
                    CHECK(zcRead.first || nDim < 3) << icShape << " zc not provided\n";
                    auto radiusRead = initialConditions.dparam( this->prefix(), "shapes", icShape, "radius" );
                    CHECK(radiusRead.first) << icShape << " radius not provided\n";

                    shapeParameterMap["id"] = icShape;
                    shapeParameterMap["xc"] = xcRead.second;
                    shapeParameterMap["yc"] = ycRead.second;
                    shapeParameterMap["zc"] = zcRead.first ? zcRead.second : 0.;
                    shapeParameterMap["radius"] = radiusRead.second;
                }
                break;

                case ShapeType::ELLIPSE:
                {
                    auto xcRead = initialConditions.dparam( this->prefix(), "shapes", icShape, "xc" );
                    CHECK(xcRead.first) << icShape << " xc not provided\n";
                    auto ycRead = initialConditions.dparam( this->prefix(), "shapes", icShape, "yc" );
                    CHECK(ycRead.first) << icShape << " yc not provided\n";
                    auto zcRead = initialConditions.dparam( this->prefix(), "shapes", icShape, "zc" );
                    CHECK(zcRead.first || nDim < 3) << icShape << " zc not provided\n";
                    auto aRead = initialConditions.dparam( this->prefix(), "shapes", icShape, "a" );
                    CHECK(aRead.first) << icShape << " a not provided\n";
                    auto bRead = initialConditions.dparam( this->prefix(), "shapes", icShape, "b" );
                    CHECK(bRead.first) << icShape << " b not provided\n";
                    auto cRead = initialConditions.dparam( this->prefix(), "shapes", icShape, "c" );
                    CHECK(cRead.first || nDim < 3) << icShape << " c not provided\n";
                    auto psiRead = initialConditions.dparam( this->prefix(), "shapes", icShape, "psi" );
                    CHECK(psiRead.first) << icShape << " psi not provided\n";
                    auto thetaRead = initialConditions.dparam( this->prefix(), "shapes", icShape, "theta" );
                    CHECK(thetaRead.first || nDim < 3) << icShape << " theta not provided\n";

                    shapeParameterMap["id"] = icShape;
                    shapeParameterMap["xc"] = xcRead.second;
                    shapeParameterMap["yc"] = ycRead.second;
                    shapeParameterMap["zc"] = zcRead.first ? zcRead.second : 0.;
                    shapeParameterMap["a"] = aRead.second;
                    shapeParameterMap["b"] = bRead.second;
                    shapeParameterMap["c"] = cRead.first ? cRead.second : 1.;
                    shapeParameterMap["psi"] = psiRead.second;
                    shapeParameterMap["theta"] = thetaRead.first ? thetaRead.second : 0.;
                }
                break;
            }

            M_icShapes.push_back( std::make_pair(shapeTypeIt->second, shapeParameterMap) );
        }
        else
        {
            CHECK(false) << "invalid shape type in " << icShape << std::endl;
        }
    } 
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
void
LEVELSETBASE_CLASS_TEMPLATE_TYPE::loadConfigPostProcess()
{
    auto& modelPostProcess = this->modelProperties().postProcess();

    if ( auto physicalQuantities = modelPostProcess.pTree().get_child_optional("PhysicalQuantities") )
    {
        for( auto& i: *physicalQuantities )
        {
            auto o = i.second.template get_value<std::string>();
            LOG(INFO) << "add to postprocess physical quantity " << o;
            if( o == "volume" || o == "all" )
                this->M_postProcessMeasuresExported.insert( LevelSetMeasuresExported::Volume );
            if( o == "perimeter" || o == "all" )
                this->M_postProcessMeasuresExported.insert( LevelSetMeasuresExported::Perimeter );
            if( o == "position_com" || o == "all" )
                this->M_postProcessMeasuresExported.insert( LevelSetMeasuresExported::Position_COM );
        }
    }

    // Load Fields from JSON
    for ( auto const& o :  modelPostProcess.exports().fields() )
    {
        if( o == "gradphi" || o == "all" )
            this->M_postProcessFieldsExported.insert( LevelSetFieldsExported::GradPhi );
        if( o == "modgradphi" || o == "all" )
            this->M_postProcessFieldsExported.insert( LevelSetFieldsExported::ModGradPhi );
        if( o == "distance" || o == "all" )
            this->M_postProcessFieldsExported.insert( LevelSetFieldsExported::Distance );
        if( o == "distance-normal" || o == "all" )
            this->M_postProcessFieldsExported.insert( LevelSetFieldsExported::DistanceNormal );
        if( o == "distance-curvature" || o == "all" )
            this->M_postProcessFieldsExported.insert( LevelSetFieldsExported::DistanceCurvature );
    }

    // Overwrite with options from CFG
    if ( Environment::vm().count(prefixvm(this->prefix(),"do_export_gradphi").c_str()) )
        if ( boption(_name="do_export_gradphi",_prefix=this->prefix()) )
            this->M_postProcessFieldsExported.insert( LevelSetFieldsExported::GradPhi );
    if ( Environment::vm().count(prefixvm(this->prefix(),"do_export_modgradphi").c_str()) )
        if ( boption(_name="do_export_modgradphi",_prefix=this->prefix()) )
            this->M_postProcessFieldsExported.insert( LevelSetFieldsExported::ModGradPhi );
    if ( Environment::vm().count(prefixvm(this->prefix(),"do_export_distance").c_str()) )
        if ( boption(_name="do_export_distance",_prefix=this->prefix()) )
            this->M_postProcessFieldsExported.insert( LevelSetFieldsExported::Distance );
    if ( Environment::vm().count(prefixvm(this->prefix(),"do_export_distancenormal").c_str()) )
        if ( boption(_name="do_export_distancenormal",_prefix=this->prefix()) )
            this->M_postProcessFieldsExported.insert( LevelSetFieldsExported::DistanceNormal );
    if ( Environment::vm().count(prefixvm(this->prefix(),"do_export_distancecurvature").c_str()) )
        if ( boption(_name="do_export_distancecurvature",_prefix=this->prefix()) )
            this->M_postProcessFieldsExported.insert( LevelSetFieldsExported::DistanceCurvature );
}

//----------------------------------------------------------------------------//
//----------------------------------------------------------------------------//
//----------------------------------------------------------------------------//
// Update levelset-dependent functions
LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
void
LEVELSETBASE_CLASS_TEMPLATE_TYPE::updateGradPhi()
{
    this->log("LevelSet", "updateGradPhi", "start");
    this->timerTool("UpdateInterfaceData").start();

    *M_levelsetGradPhi = this->grad( this->phiElt(), M_gradPhiMethod );

    M_doUpdateGradPhi = false;
    double timeElapsed = this->timerTool("UpdateInterfaceData").stop();
    this->log("LevelSet", "updateGradPhi", "finish in "+(boost::format("%1% s") %timeElapsed).str() );
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
void
LEVELSETBASE_CLASS_TEMPLATE_TYPE::updateModGradPhi()
{
    this->log("LevelSet", "updateModGradPhi", "start");
    this->timerTool("UpdateInterfaceData").start();

    *M_levelsetModGradPhi = this->modGrad( this->phiElt(), M_modGradPhiMethod );

    M_doUpdateModGradPhi = false;

    double timeElapsed = this->timerTool("UpdateInterfaceData").stop();
    this->log("LevelSet", "updateModGradPhi", "finish in "+(boost::format("%1% s") %timeElapsed).str() );
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
void
LEVELSETBASE_CLASS_TEMPLATE_TYPE::updateDirac()
{
    this->log("LevelSet", "updateDirac", "start");
    this->timerTool("UpdateInterfaceData").start();

    auto eps0 = this->thicknessInterface();

    //if( M_useAdaptiveThicknessInterface )
    //{
        //auto gradPhi = this->gradPhi();
        //auto gradPhiX = vf::project(
                //_space=this->functionSpace(),
                //_range=this->rangeMeshElements(),
                //_expr=idv(gradPhi->comp(Component::X))
                //);
        //auto gradPhiY = vf::project(
                //_space=this->functionSpace(),
                //_range=this->rangeMeshElements(),
                //_expr=idv(gradPhi->comp(Component::Y))
                //);
//#if FEELPP_DIM == 3
        //auto gradPhiZ = vf::project(
                //_space=this->functionSpace(),
                //_range=this->rangeMeshElements(),
                //_expr=idv(gradPhi->comp(Component::Z))
                //);
//#endif
        //auto eps_elt = this->functionSpace()->element();
        //eps_elt = vf::project(
                //_space=this->functionSpace(),
                //_range=this->rangeMeshElements(),
                //_expr=(vf::abs(idv(gradPhiX))+vf::abs(idv(gradPhiY))
//#if FEELPP_DIM == 3
                    //+ vf::abs(idv(gradPhiZ))
//#endif
                    //)*cst(eps0)/idv(this->modGradPhi())
                //);

        //auto eps = idv(eps_elt);
        //auto psi = idv(this->phi());

        //if ( M_useHeavisideDiracNodalProj )
            //*M_dirac = vf::project( this->functionSpace(), this->rangeMeshElements(),
                    //Feel::FeelModels::levelsetDelta(psi, eps) );
        //else
            //*M_dirac = M_projectorL2Scalar->project( Feel::FeelModels::levelsetDelta(psi, eps) );
    //}
    auto const& psi = this->phiElt();

    if ( M_useHeavisideDiracNodalProj )
        *M_dirac = vf::project( this->functionSpace(), this->rangeMeshElements(),
                this->diracExpr() );
    else
        *M_dirac = M_projectorL2Scalar->project( this->diracExpr() );

    M_doUpdateDirac = false;

    double timeElapsed = this->timerTool("UpdateInterfaceData").stop();
    this->log("LevelSet", "updateDirac", "finish in "+(boost::format("%1% s") %timeElapsed).str() );
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
void
LEVELSETBASE_CLASS_TEMPLATE_TYPE::updateHeaviside()
{ 
    this->log("LevelSet", "updateHeaviside", "start");
    this->timerTool("UpdateInterfaceData").start();

    auto eps = this->thicknessInterface();

    if (M_useRegularPhi)
    {
        auto psi = idv(this->phiElt()) / idv(this->modGradPhi());

        if ( M_useHeavisideDiracNodalProj )
            *M_heaviside = vf::project( this->functionSpace(), this->rangeMeshElements(),
                   Feel::FeelModels::levelsetHeaviside(psi, cst(eps)) );
        else
            *M_heaviside = M_projectorL2Scalar->project( Feel::FeelModels::levelsetHeaviside(psi, cst(eps)) );
    }
    else
    {
        auto psi = idv(this->phiElt());

        if ( M_useHeavisideDiracNodalProj )
            *M_heaviside = vf::project( this->functionSpace(), this->rangeMeshElements(),
                   Feel::FeelModels::levelsetHeaviside(psi, cst(eps)) );
        else
            *M_heaviside = M_projectorL2Scalar->project( Feel::FeelModels::levelsetHeaviside(psi, cst(eps)) );
    }

    M_doUpdateHeaviside = false;

    double timeElapsed = this->timerTool("UpdateInterfaceData").stop();
    this->log("LevelSet", "updateHeaviside", "finish in "+(boost::format("%1% s") %timeElapsed).str() );
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
void
LEVELSETBASE_CLASS_TEMPLATE_TYPE::updatePhiPN()
{
    this->log("LevelSet", "updatePhiPN", "start");
    this->timerTool("UpdateInterfaceData").start();

    auto const& phi = this->phiElt();
    this->functionSpaceManager()->opInterpolationScalarToPN()->apply( phi, *M_levelsetPhiPN );
    //*M_levelsetPhiPN = M_projectorL2P1PN->project(
            //_expr=idv(phi)
            //);

    M_doUpdatePhiPN = false;

    double timeElapsed = this->timerTool("UpdateInterfaceData").stop();
    this->log("LevelSet", "updatePhiPN", "finish in "+(boost::format("%1% s") %timeElapsed).str() );
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
void
LEVELSETBASE_CLASS_TEMPLATE_TYPE::updateNormal()
{
    this->log("LevelSet", "updateNormal", "start");
    this->timerTool("UpdateInterfaceData").start();

    //auto const& phi = this->phiElt();
    //*M_levelsetNormal = M_projectorL2Vectorial->project( _expr=trans(gradv(phi)) / sqrt(gradv(phi) * trans(gradv(phi))) );
    auto gradPhi = this->gradPhi();
    *M_levelsetNormal = vf::project( 
            _space=this->functionSpaceVectorial(),
            _range=this->rangeMeshElements(),
            //_expr=trans(gradv(phi)) / sqrt(gradv(phi) * trans(gradv(phi))) 
            _expr=idv(gradPhi) / sqrt(trans(idv(gradPhi)) * idv(gradPhi)) 
            );

    M_doUpdateNormal = false;

    double timeElapsed = this->timerTool("UpdateInterfaceData").stop();
    this->log("LevelSet", "updateNormal", "finish in "+(boost::format("%1% s") %timeElapsed).str() );
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
void
LEVELSETBASE_CLASS_TEMPLATE_TYPE::updateCurvature()
{
    this->log("LevelSet", "updateCurvature", "start");
    this->timerTool("UpdateInterfaceData").start();

    switch( M_curvatureMethod )
    {
        case CurvatureMethod::NODAL_PROJECTION:
        {
            this->log("LevelSet", "updateCurvature", "perform nodal projection");
            M_levelsetCurvature->on( _range=this->rangeMeshElements(), _expr=divv(this->normal()) );
        }
        break;
        case CurvatureMethod::L2_PROJECTION:
        {
            this->log("LevelSet", "updateCurvature", "perform L2 projection");
            //*M_levelsetCurvature = this->projectorL2()->project( _expr=divv(this->normal()) );
            *M_levelsetCurvature = this->projectorL2()->derivate( trans(idv(this->normal())) );
        }
        break;
        case CurvatureMethod::SMOOTH_PROJECTION:
        {
            this->log("LevelSet", "updateCurvature", "perform smooth projection");
            *M_levelsetCurvature = this->smoother()->project( _expr=divv(this->normal()) );
        }
        break;
        case CurvatureMethod::PN_NODAL_PROJECTION:
        {
            this->log("LevelSet", "updateCurvature", "perform PN-nodal projection");
            auto phiPN = this->phiPN();
            auto normalPN = vf::project(
                    _space=this->functionSpaceManager()->functionSpaceVectorialPN(),
                    _range=this->functionSpaceManager()->rangeMeshPNElements(),
                    _expr=trans(gradv(phiPN)) / sqrt(gradv(phiPN)*trans(gradv(phiPN)))
                    );
            auto curvaturePN = vf::project(
                    _space=this->functionSpaceManager()->functionSpaceScalarPN(),
                    _range=this->functionSpaceManager()->rangeMeshPNElements(),
                    _expr=divv(normalPN)
                    );

            this->functionSpaceManager()->opInterpolationScalarFromPN()->apply( curvaturePN, *M_levelsetCurvature );
        }
        break;
        case CurvatureMethod::DIFFUSION_ORDER1:
        {
            this->log("LevelSet", "updateCurvature", "perform diffusion order1");
            *M_levelsetCurvature = this->toolManager()->curvatureDiffusion()->curvatureOrder1( this->distance() );
        }
        break;
        case CurvatureMethod::DIFFUSION_ORDER2:
        {
            this->log("LevelSet", "updateCurvature", "perform diffusion order2");
            *M_levelsetCurvature = this->toolManager()->curvatureDiffusion()->curvatureOrder2( this->distance() );
        }
        break;
    }

    M_doUpdateCurvature = false;

    double timeElapsed = this->timerTool("UpdateInterfaceData").stop();
    this->log("LevelSet", "updateCurvature", "finish in "+(boost::format("%1% s") %timeElapsed).str() );
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
void
LEVELSETBASE_CLASS_TEMPLATE_TYPE::updateDistance()
{
    this->log("LevelSet", "updateDistance", "start");
    this->timerTool("UpdateInterfaceData").start();

    *M_distance = this->redistantiate( this->phiElt(), M_distanceMethod );

    M_doUpdateDistance = false;

    double timeElapsed = this->timerTool("UpdateInterfaceData").stop();
    this->log("LevelSet", "updateDistance", "finish in "+(boost::format("%1% s") %timeElapsed).str() );
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
void
LEVELSETBASE_CLASS_TEMPLATE_TYPE::updateDistanceNormal()
{
    this->log("LevelSet", "updateDistanceNormal", "start");
    this->timerTool("UpdateInterfaceData").start();

    auto const& phi = this->distance();
    auto const N_expr = trans(gradv(phi)) / sqrt( gradv(phi)*trans(gradv(phi)) );

    switch( M_gradPhiMethod )
    {
        case DerivationMethod::NODAL_PROJECTION:
            this->log("LevelSet", "updateDistanceNormal", "perform nodal projection");
            M_distanceNormal->on( _range=this->rangeMeshElements(), _expr=N_expr );
            break;
        case DerivationMethod::L2_PROJECTION:
            this->log("LevelSet", "updateDistanceNormal", "perform L2 projection");
            *M_distanceNormal = this->projectorL2Vectorial()->project( N_expr );
            break;
        case DerivationMethod::SMOOTH_PROJECTION:
            this->log("LevelSet", "updateDistanceNormal", "perform smooth projection");
            *M_distanceNormal = this->smootherVectorial()->project( N_expr );
            break;
        case DerivationMethod::PN_NODAL_PROJECTION:
            this->log("LevelSet", "updateDistanceNormal", "perform PN-nodal projection");
            CHECK( false ) << "TODO: updateDistanceNormal with PN_NODAL_PROJECTION method\n";
            //auto phiPN = this->phiPN();
            //auto gradPhiPN = vf::project(
                    //_space=this->functionSpaceManager()->functionSpaceVectorialPN(),
                    //_range=this->functionSpaceManager()->rangeMeshPNElements(),
                    //_expr=trans(gradv(phiPN))
                    //);
            //this->functionSpaceManager()->opInterpolationVectorialFromPN()->apply( gradPhiPN, *M_distanceNormal );
            break;
    }

    M_doUpdateDistanceNormal = false;

    double timeElapsed = this->timerTool("UpdateInterfaceData").stop();
    this->log("LevelSet", "updateDistanceNormal", "finish in "+(boost::format("%1% s") %timeElapsed).str() );
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
void
LEVELSETBASE_CLASS_TEMPLATE_TYPE::updateDistanceCurvature()
{
    this->log("LevelSet", "updateDistanceCurvature", "start");
    this->timerTool("UpdateInterfaceData").start();

    switch( M_curvatureMethod )
    {
        case CurvatureMethod::NODAL_PROJECTION:
        {
            this->log("LevelSet", "updateDistanceCurvature", "perform nodal projection");
            M_distanceCurvature->on( _range=this->rangeMeshElements(), _expr=divv(this->distanceNormal()) );
        }
        break;
        case CurvatureMethod::L2_PROJECTION:
        {
            this->log("LevelSet", "updateDistanceCurvature", "perform L2 projection");
            //*M_distanceCurvature = this->projectorL2()->project( _expr=divv(this->distanceNormal()) );
            *M_distanceCurvature = this->projectorL2()->derivate( trans(idv(this->distanceNormal())) );
        }
        break;
        case CurvatureMethod::SMOOTH_PROJECTION:
        {
            this->log("LevelSet", "updateDistanceCurvature", "perform smooth projection");
            *M_distanceCurvature = this->smoother()->project( _expr=divv(this->distanceNormal()) );
        }
        break;
        case CurvatureMethod::PN_NODAL_PROJECTION:
        {
            this->log("LevelSet", "updateDistanceCurvature", "perform PN-nodal projection");
            CHECK( false ) << "TODO: updateDistanceCurvature with PN_NODAL_PROJECTION method\n";
            //auto phiPN = this->phiPN();
            //auto normalPN = vf::project(
                    //_space=this->functionSpaceManager()->functionSpaceVectorialPN(),
                    //_range=this->functionSpaceManager()->rangeMeshPNElements(),
                    //_expr=trans(gradv(phiPN)) / sqrt(gradv(phiPN)*trans(gradv(phiPN)))
                    //);
            //auto curvaturePN = vf::project(
                    //_space=this->functionSpaceManager()->functionSpaceScalarPN(),
                    //_range=this->functionSpaceManager()->rangeMeshPNElements(),
                    //_expr=divv(normalPN)
                    //);

            //this->functionSpaceManager()->opInterpolationScalarFromPN()->apply( curvaturePN, *M_distanceCurvature );
        }
        break;
        case CurvatureMethod::DIFFUSION_ORDER1:
        {
            this->log("LevelSet", "updateDistanceCurvature", "perform diffusion order1");
            *M_distanceCurvature = this->toolManager()->curvatureDiffusion()->curvatureOrder1( this->distance() );
        }
        break;
        case CurvatureMethod::DIFFUSION_ORDER2:
        {
            this->log("LevelSet", "updateDistanceCurvature", "perform diffusion order2");
            *M_distanceCurvature = this->toolManager()->curvatureDiffusion()->curvatureOrder2( this->distance() );
        }
        break;
    }

    M_doUpdateDistanceCurvature = false;

    double timeElapsed = this->timerTool("UpdateInterfaceData").stop();
    this->log("LevelSet", "updateDistanceCurvature", "finish in "+(boost::format("%1% s") %timeElapsed).str() );
}

//----------------------------------------------------------------------------//
//----------------------------------------------------------------------------//
//----------------------------------------------------------------------------//

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
typename LEVELSETBASE_CLASS_TEMPLATE_TYPE::projector_levelset_ptrtype const&
LEVELSETBASE_CLASS_TEMPLATE_TYPE::smootherInterface() const
{
    if( !M_smootherInterface || M_doUpdateSmootherInterface )
    {
        auto const spaceInterface = self_type::space_levelset_type::New( 
                _mesh=this->mesh(),
                _range=this->rangeDiracElements(),
                _worldscomm=this->worldsComm()
                );
        M_smootherInterface = Feel::projector( 
                spaceInterface, spaceInterface,
                backend(_name=prefixvm(this->prefix(),"smoother"), _worldcomm=this->worldCommPtr(), _rebuild=true), 
                DIFF,
                this->mesh()->hAverage()*doption(_name="smooth-coeff", _prefix=prefixvm(this->prefix(),"smoother"))/Order,
                30);
        M_doUpdateSmootherInterface = false;
    }
    return M_smootherInterface;
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
typename LEVELSETBASE_CLASS_TEMPLATE_TYPE::projector_levelset_vectorial_ptrtype const&
LEVELSETBASE_CLASS_TEMPLATE_TYPE::smootherInterfaceVectorial() const
{
    if( !M_smootherInterfaceVectorial || M_doUpdateSmootherInterfaceVectorial )
    {
        auto const spaceInterfaceVectorial = self_type::space_vectorial_type::New( 
                _mesh=this->mesh(),
                _range=this->rangeDiracElements(),
                _worldscomm=this->worldsComm()
                );
        M_smootherInterfaceVectorial = Feel::projector(
                spaceInterfaceVectorial, spaceInterfaceVectorial,
                backend(_name=prefixvm(this->prefix(),"smoother-vec"), _worldcomm=this->worldCommPtr(), _rebuild=true),
                DIFF, 
                this->mesh()->hAverage()*doption(_name="smooth-coeff", _prefix=prefixvm(this->prefix(),"smoother-vec"))/Order,
                30);
        M_doUpdateSmootherInterfaceVectorial = false;
    }
    return M_smootherInterfaceVectorial;
}
//----------------------------------------------------------------------------//
//----------------------------------------------------------------------------//
//----------------------------------------------------------------------------//
// Markers accessors
LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
typename LEVELSETBASE_CLASS_TEMPLATE_TYPE::element_markers_ptrtype const&
LEVELSETBASE_CLASS_TEMPLATE_TYPE::markerInterface() const
{
    if( !M_markerInterface )
        M_markerInterface.reset( new element_markers_type(M_spaceMarkers, "MarkerInterface") );

    if( M_doUpdateMarkers )
       const_cast<self_type*>(this)->updateMarkerInterface(); 

    return M_markerInterface;
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
typename LEVELSETBASE_CLASS_TEMPLATE_TYPE::element_markers_ptrtype const&
LEVELSETBASE_CLASS_TEMPLATE_TYPE::markerDirac() const
{
    if( !M_markerDirac )
        M_markerDirac.reset( new element_markers_type(M_spaceMarkers, "MarkerDirac") );

    if( M_doUpdateMarkers )
       const_cast<self_type*>(this)->updateMarkerDirac(); 

    return M_markerDirac;
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
typename LEVELSETBASE_CLASS_TEMPLATE_TYPE::element_markers_ptrtype const&
LEVELSETBASE_CLASS_TEMPLATE_TYPE::markerOuter( double cut ) const
{
    if( !M_markerOuter )
        M_markerOuter.reset( new element_markers_type(M_spaceMarkers, "MarkerOuter") );

    if( M_doUpdateMarkers || cut != M_markerOuterCut )
    {
       const_cast<self_type*>(this)->markerHeavisideImpl( M_markerOuter, false, cut );
       M_markerOuterCut = cut;
    }

    return M_markerOuter;
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
typename LEVELSETBASE_CLASS_TEMPLATE_TYPE::element_markers_ptrtype const&
LEVELSETBASE_CLASS_TEMPLATE_TYPE::markerInner( double cut ) const
{
    if( !M_markerInner )
        M_markerInner.reset( new element_markers_type(M_spaceMarkers, "MarkerInner") );

    if( M_doUpdateMarkers || cut != M_markerInnerCut )
    {
       const_cast<self_type*>(this)->markerHeavisideImpl( M_markerInner, true, cut );
       M_markerInnerCut = cut;
    }

    return M_markerInner;
}

//LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
//typename LEVELSETBASE_CLASS_TEMPLATE_TYPE::element_markers_ptrtype const&
//LEVELSETBASE_CLASS_TEMPLATE_TYPE::markerCrossedElements() const
//{
    //if( !M_markerCrossedElements )
        //M_markerCrossedElements.reset( new element_markers_type(M_spaceMarkers, "MarkerCrossedElements") );

    //if( M_doUpdateMarkers )
       //const_cast<self_type*>(this)->updateMarkerCrossedElements(); 

    //return M_markerCrossedElements;
//}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
typename LEVELSETBASE_CLASS_TEMPLATE_TYPE::range_elements_type const&
LEVELSETBASE_CLASS_TEMPLATE_TYPE::rangeInterfaceElements() const
{
    if( this->M_doUpdateInterfaceElements )
    {
        M_interfaceElements = this->rangeInterfaceElementsImpl( this->phiElt() );
        M_doUpdateInterfaceElements = false;
    }

    return M_interfaceElements;
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
typename LEVELSETBASE_CLASS_TEMPLATE_TYPE::range_elements_type
LEVELSETBASE_CLASS_TEMPLATE_TYPE::rangeInterfaceElementsImpl( element_levelset_type const& phi ) const
{
    mesh_ptrtype const& mesh = this->mesh();

    auto it_elt = mesh->beginOrderedElement();
    auto en_elt = mesh->endOrderedElement();

    const rank_type pid = mesh->worldCommElements().localRank();
    const int ndofv = space_levelset_type::fe_type::nDof;

    elements_reference_wrapper_ptrtype interfaceElts( new elements_reference_wrapper_type );

    for (; it_elt!=en_elt; it_elt++)
    {
        auto const& elt = boost::unwrap_ref( *it_elt );
        if ( elt.processId() != pid )
            continue;
        bool mark_elt = false;
        bool hasPositivePhi = false;
        bool hasNegativePhi = false;
        for (int j=0; j<ndofv; j++)
        {
            if ( phi.localToGlobal(elt.id(), j, 0) < 0. )
            {
                // phi < 0
                if( hasPositivePhi )
                {
                    mark_elt = true;
                    break; //don't need to do the others dof
                }
                hasNegativePhi = true;
            }
            if ( phi.localToGlobal(elt.id(), j, 0) > 0. )
            {
                // phi > 0
                if( hasNegativePhi )
                {
                    mark_elt = true;
                    break; //don't need to do the others dof
                }
                hasPositivePhi = true;
            }
        }
        if( mark_elt )
            interfaceElts->push_back( boost::cref(elt) );
    }

    return boost::make_tuple( mpl::size_t<MESH_ELEMENTS>(),
            interfaceElts->begin(),
            interfaceElts->end(),
            interfaceElts
            );
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
typename LEVELSETBASE_CLASS_TEMPLATE_TYPE::range_elements_type
LEVELSETBASE_CLASS_TEMPLATE_TYPE::rangeThickInterfaceElementsImpl( element_levelset_type const& phi, double thickness ) const
{
    mesh_ptrtype const& mesh = this->mesh();

    auto it_elt = mesh->beginOrderedElement();
    auto en_elt = mesh->endOrderedElement();

    const rank_type pid = mesh->worldCommElements().localRank();
    const int ndofv = space_levelset_type::fe_type::nDof;

    elements_reference_wrapper_ptrtype interfaceElts( new elements_reference_wrapper_type );

    for (; it_elt!=en_elt; it_elt++)
    {
        auto const& elt = boost::unwrap_ref( *it_elt );
        if ( elt.processId() != pid )
            continue;
        bool mark_elt = false;
        for (int j=0; j<ndofv; j++)
        {
            if ( std::abs(phi.localToGlobal(elt.id(), j, 0)) <= thickness )
            {
                mark_elt = true;
                break; //don't need to do the others dof
            }
        }
        if( mark_elt )
            interfaceElts->push_back( boost::cref(elt) );
    }

    return boost::make_tuple( mpl::size_t<MESH_ELEMENTS>(),
            interfaceElts->begin(),
            interfaceElts->end(),
            interfaceElts
            );
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
typename LEVELSETBASE_CLASS_TEMPLATE_TYPE::range_elements_type
LEVELSETBASE_CLASS_TEMPLATE_TYPE::rangeOuterElements( double cut ) const
{
    mesh_ptrtype const& mesh = this->mesh();

    element_levelset_type phi = this->functionSpace()->element();
    if( this->M_useRegularPhi )
        phi.on( _range=this->rangeMeshElements(), _expr=idv(this->phiElt()) / idv(this->modGradPhi()) );
    else
        phi = this->phiElt();

    auto it_elt = mesh->beginOrderedElement();
    auto en_elt = mesh->endOrderedElement();

    const rank_type pid = mesh->worldCommElements().localRank();
    const int ndofv = space_levelset_type::fe_type::nDof;

    elements_reference_wrapper_ptrtype outerElts( new elements_reference_wrapper_type );

    for (; it_elt!=en_elt; it_elt++)
    {
        auto const& elt = boost::unwrap_ref( *it_elt );
        if ( elt.processId() != pid )
            continue;
        bool mark_elt = true;
        for (int j=0; j<ndofv; j++)
        {
            if ( phi.localToGlobal(elt.id(), j, 0) < cut )
            {
                mark_elt = false;
                break; //don't need to do the others dof
            }
        }
        if( mark_elt )
            outerElts->push_back( boost::cref(elt) );
    }

    range_elements_type outerElements = boost::make_tuple( 
            mpl::size_t<MESH_ELEMENTS>(),
            outerElts->begin(),
            outerElts->end(),
            outerElts
            );

    return outerElements;
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
typename LEVELSETBASE_CLASS_TEMPLATE_TYPE::range_elements_type const&
LEVELSETBASE_CLASS_TEMPLATE_TYPE::rangeDiracElements() const
{
    if( M_doUpdateRangeDiracElements )
    {
        mesh_ptrtype const& mesh = this->mesh();

        auto it_elt = mesh->beginOrderedElement();
        auto en_elt = mesh->endOrderedElement();

        const rank_type pid = mesh->worldCommElements().localRank();
        const int ndofv = space_levelset_type::fe_type::nDof;

        double thickness = 2*this->thicknessInterface();
        elements_reference_wrapper_ptrtype diracElts( new elements_reference_wrapper_type );

        for (; it_elt!=en_elt; it_elt++)
        {
            auto const& elt = boost::unwrap_ref( *it_elt );
            if ( elt.processId() != pid )
                continue;
            bool mark_elt = false;
            for (int j=0; j<ndofv; j++)
            {
                if ( this->dirac()->localToGlobal(elt.id(), j, 0) > 0. )
                {
                    mark_elt = true;
                    break; //don't need to do the others dof
                }
            }
            if( mark_elt )
                diracElts->push_back( boost::cref(elt) );
        }

        M_rangeDiracElements = boost::make_tuple( mpl::size_t<MESH_ELEMENTS>(),
                diracElts->begin(),
                diracElts->end(),
                diracElts
                );

        M_doUpdateRangeDiracElements = false;
    }

    return M_rangeDiracElements;
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
typename LEVELSETBASE_CLASS_TEMPLATE_TYPE::range_faces_type
LEVELSETBASE_CLASS_TEMPLATE_TYPE::rangeInterfaceFaces() const
{
    CHECK( Environment::isSequential() ) << "There is a bug to be fixed in parallel. Only run in sequential at the moment...\n";

    if( this->M_doUpdateInterfaceFaces )
    {
        mesh_ptrtype const& mesh = this->mesh();
        auto& markerIn = this->markerInner();
        markerIn->close();
        CHECK( markerIn->functionSpace()->extendedDofTable() ) << "interfaceFaces needs extended doftable in markers function space\n";

        auto it_face = mesh->beginFace();
        auto en_face = mesh->endFace();

        const rank_type pid = mesh->worldCommFaces().localRank();

        faces_reference_wrapper_ptrtype interfaceFaces( new faces_reference_wrapper_type );

        for (; it_face!=en_face; it_face++)
        {
            auto const& curFace = it_face->second; 
            if ( curFace.processId() != pid )
                continue;
            if ( !(curFace.isConnectedTo0() && curFace.isConnectedTo1()) )
                continue;
            bool isInnerElt0 = (markerIn->localToGlobal( curFace.element0().id(), 0, 0 ) > 1e-3);
            bool isInnerElt1 = (markerIn->localToGlobal( curFace.element1().id(), 0, 0 ) > 1e-3);

            if( (isInnerElt0 && !isInnerElt1) || (!isInnerElt0 && isInnerElt1) )
            {
                interfaceFaces->push_back( boost::cref(curFace) );
            }
        }

        M_interfaceFaces = boost::make_tuple( mpl::size_t<MESH_FACES>(),
                interfaceFaces->begin(),
                interfaceFaces->end(),
                interfaceFaces
                );

        M_doUpdateInterfaceFaces = false;
    }

    return M_interfaceFaces;
}

//----------------------------------------------------------------------------//
// Utility distances
LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
typename LEVELSETBASE_CLASS_TEMPLATE_TYPE::element_levelset_ptrtype
LEVELSETBASE_CLASS_TEMPLATE_TYPE::distToBoundary()
{
    element_levelset_ptrtype distToBoundary( new element_levelset_type(this->functionSpace(), "DistToBoundary") );

    // Retrieve the elements touching the boundary
    auto boundaryelts = boundaryelements( this->mesh() );

    // Mark the elements in myrange
    auto mymark = vf::project(
            _space=this->functionSpaceMarkers(),
            _range=boundaryelts,
            _expr=cst(1)
            );

    // Update mesh marker2
    this->mesh()->updateMarker2( mymark );

    // Init phi0 with h on marked2 elements
    auto phi0 = vf::project(
            _space=this->functionSpace(),
            _range=boundaryelts,
            _expr=h()
            );
    phi0.on( _range=boundaryfaces(this->mesh()), _expr=cst(0.) );

    // Run FM using marker2 as marker DONE
    this->reinitializerFM()->setUseMarker2AsMarkerDone( true );
    *distToBoundary = this->reinitializerFM()->run( phi0 );

    return distToBoundary;
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
typename LEVELSETBASE_CLASS_TEMPLATE_TYPE::element_levelset_ptrtype
LEVELSETBASE_CLASS_TEMPLATE_TYPE::distToMarkedFaces( boost::any const& marker )
{
    element_levelset_ptrtype distToMarkedFaces( new element_levelset_type(this->functionSpace(), "DistToMarkedFaces") );

    typedef boost::reference_wrapper<typename MeshTraits<mymesh_type>::element_type const> element_ref_type;
    typedef std::vector<element_ref_type> cont_range_type;
    std::shared_ptr<cont_range_type> myelts( new cont_range_type );

    // Retrieve the elements touching the marked faces
    auto mfaces = markedfaces( this->mesh(), marker );
    for( auto const& faceWrap: mfaces )
    {
        auto const& face = unwrap_ref( faceWrap );
        if( face.isConnectedTo0() )
            myelts->push_back(boost::cref(face.element0()));
        if( face.isConnectedTo1() )
            myelts->push_back(boost::cref(face.element1()));
    }

    auto myrange = boost::make_tuple(
            mpl::size_t<MESH_ELEMENTS>(), myelts->begin(), myelts->end(), myelts
            );

    // Mark the elements in myrange
    auto mymark = this->functionSpaceMarkers()->element();
    mymark.on( _range=myrange, _expr=cst(1) );

    // Update mesh marker2
    this->mesh()->updateMarker2( mymark );

    // Init phi0 with h on marked2 elements
    auto phi0 = vf::project(
            _space=this->functionSpace(),
            _range=myrange,
            _expr=h()
            );
    phi0.on( _range=mfaces, _expr=cst(0.) );

    // Run FM using marker2 as marker DONE
    this->reinitializerFM()->setUseMarker2AsMarkerDone( true );
    *distToMarkedFaces = this->reinitializerFM()->run( phi0 );

    return distToMarkedFaces;
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
typename LEVELSETBASE_CLASS_TEMPLATE_TYPE::element_levelset_ptrtype
LEVELSETBASE_CLASS_TEMPLATE_TYPE::distToMarkedFaces( std::initializer_list<boost::any> marker )
{
    element_levelset_ptrtype distToMarkedFaces( new element_levelset_type(this->functionSpace(), "DistToMarkedFaces") );

    typedef boost::reference_wrapper<typename MeshTraits<mymesh_type>::element_type const> element_ref_type;
    typedef std::vector<element_ref_type> cont_range_type;
    std::shared_ptr<cont_range_type> myelts( new cont_range_type );

    // Retrieve the elements touching the marked faces
    //auto mfaces_list = markedfaces( this->mesh(), marker );
    auto mfaces = markedfaces( this->mesh(), marker );
    for( auto const& faceWrap: mfaces )
    {
        auto const& face = unwrap_ref( faceWrap );
        if( face.isConnectedTo0() )
            myelts->push_back(boost::cref(face.element0()));
        if( face.isConnectedTo1() )
            myelts->push_back(boost::cref(face.element1()));
    }

    auto myrange = boost::make_tuple(
            mpl::size_t<MESH_ELEMENTS>(), myelts->begin(), myelts->end(), myelts
            );

    // Mark the elements in myrange
    auto mymark = this->functionSpaceMarkers()->element();
    mymark.on( _range=myrange, _expr=cst(1) );

    // Update mesh marker2
    this->mesh()->updateMarker2( mymark );

    // Init phi0 with h on marked2 elements
    auto phi0 = vf::project(
            _space=this->functionSpace(),
            _range=myrange,
            _expr=h()
            );
    phi0.on( _range=mfaces, _expr=cst(0.) );

    // Run FM using marker2 as marker DONE
    this->reinitializerFM()->setUseMarker2AsMarkerDone( true );
    *distToMarkedFaces = this->reinitializerFM()->run( phi0 );

    return distToMarkedFaces;
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
void
LEVELSETBASE_CLASS_TEMPLATE_TYPE::updateInterfaceQuantities()
{
    M_doUpdateDirac = true;
    M_doUpdateHeaviside = true;
    M_doUpdateInterfaceElements = true;
    M_doUpdateRangeDiracElements = true;
    M_doUpdateInterfaceFaces = true;
    M_doUpdateSmootherInterface = true;
    M_doUpdateSmootherInterfaceVectorial = true;
    M_doUpdateNormal = true;
    M_doUpdateCurvature = true;
    M_doUpdateMarkers = true;
    M_doUpdateGradPhi = true;
    M_doUpdateModGradPhi = true;
    M_doUpdatePhiPN = true;
    M_doUpdateDistance = true;
    M_doUpdateDistanceNormal = true;
    M_doUpdateDistanceCurvature = true;
    M_doUpdateSubmeshDirac = true;
    M_doUpdateSubmeshOuter = true;
    M_doUpdateSubmeshInner = true;
}

//----------------------------------------------------------------------------//
// Interface quantities helpers
LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
typename LEVELSETBASE_CLASS_TEMPLATE_TYPE::element_vectorial_type
LEVELSETBASE_CLASS_TEMPLATE_TYPE::grad( element_levelset_type const& phi, DerivationMethod method ) const
{
    switch( method )
    {
        case DerivationMethod::NODAL_PROJECTION:
            this->log("LevelSet", "grad", "perform nodal projection");
            return vf::project( 
                    _space=this->functionSpaceVectorial(),
                    _range=this->rangeMeshElements(),
                    _expr=trans(gradv(phi))
                    );
        case DerivationMethod::L2_PROJECTION:
            this->log("LevelSet", "grad", "perform L2 projection");
            //return this->projectorL2Vectorial()->project( _expr=trans(gradv(phi)) );
            return this->projectorL2Vectorial()->derivate( idv(phi) );
        case DerivationMethod::SMOOTH_PROJECTION:
            this->log("LevelSet", "grad", "perform smooth projection");
            return this->smootherVectorial()->project( trans(gradv(phi)) );
        case DerivationMethod::PN_NODAL_PROJECTION:
            this->log("LevelSet", "grad", "perform PN-nodal projection");
            CHECK( M_useSpaceIsoPN ) << "use-space-iso-pn must be enabled to use PN_NODAL_PROJECTION \n";
            auto phiPN = this->functionSpaceManager()->opInterpolationScalarToPN()->operator()( phi );
            auto gradPhiPN = vf::project(
                    _space=this->functionSpaceManager()->functionSpaceVectorialPN(),
                    _range=this->functionSpaceManager()->rangeMeshPNElements(),
                    _expr=trans(gradv(phiPN))
                    );
            return this->functionSpaceManager()->opInterpolationVectorialFromPN()->operator()( gradPhiPN );
    }
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
typename LEVELSETBASE_CLASS_TEMPLATE_TYPE::element_levelset_type
LEVELSETBASE_CLASS_TEMPLATE_TYPE::modGrad( element_levelset_type const& phi, DerivationMethod method ) const
{
    switch( method )
    {
        case DerivationMethod::NODAL_PROJECTION:
            this->log("LevelSet", "modGrad", "perform nodal projection");
            return vf::project( 
                    _space=this->functionSpace(),
                    _range=this->rangeMeshElements(),
                    _expr=sqrt( gradv(phi)*trans(gradv(phi)) )
                    );
        case DerivationMethod::L2_PROJECTION:
            this->log("LevelSet", "modGrad", "perform L2 projection");
            return this->projectorL2()->project( sqrt( gradv(phi)*trans(gradv(phi)) ) );
        case DerivationMethod::SMOOTH_PROJECTION:
            this->log("LevelSet", "modGrad", "perform smooth projection");
            return this->smoother()->project( sqrt( gradv(phi)*trans(gradv(phi)) ) );
        case DerivationMethod::PN_NODAL_PROJECTION:
            this->log("LevelSet", "modGrad", "perform PN-nodal projection");
            CHECK( M_useSpaceIsoPN ) << "use-space-iso-pn must be enabled to use PN_NODAL_PROJECTION \n";
            auto phiPN = this->functionSpaceManager()->opInterpolationScalarToPN()->operator()( phi );
            auto modGradPhiPN = vf::project(
                    _space=this->functionSpaceManager()->functionSpaceScalarPN(),
                    _range=this->functionSpaceManager()->rangeMeshPNElements(),
                    _expr=sqrt( gradv(phiPN)*trans(gradv(phiPN)) )
                    );
            return this->functionSpaceManager()->opInterpolationScalarFromPN()->operator()( modGradPhiPN );
    }
}

//----------------------------------------------------------------------------//
// Reinitialization
LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
void
LEVELSETBASE_CLASS_TEMPLATE_TYPE::reinitialize()
{ 
    this->log("LevelSet", "reinitialize", "start");
    this->timerTool("Reinit").start();

    auto phi = this->phiPtr();

    if ( M_useMarkerDiracAsMarkerDoneFM )
    {
        this->mesh()->updateMarker2( *this->markerDirac() );
    }

    *phi = this->redistantiate( *phi, M_reinitMethod );

    M_hasReinitialized = true;

    double timeElapsed = this->timerTool("Reinit").stop();
    this->log("LevelSet","reinitialize","finish in "+(boost::format("%1% s") %timeElapsed).str() );
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
typename LEVELSETBASE_CLASS_TEMPLATE_TYPE::element_levelset_type
LEVELSETBASE_CLASS_TEMPLATE_TYPE::redistantiate( element_levelset_type const& phi, LevelSetDistanceMethod method ) const
{ 
    auto phiReinit = this->functionSpace()->elementPtr();

    switch( method )
    {
        case LevelSetDistanceMethod::NONE:
        {
            *phiReinit = phi;
        }
        break;
        case LevelSetDistanceMethod::FASTMARCHING:
        {
            switch (M_fastMarchingInitializationMethod)
            {
                case FastMarchingInitializationMethod::ILP_NODAL :
                {
                    phiReinit->on( 
                            _range=this->rangeMeshElements(), 
                            _expr=idv(phi)/sqrt( inner( gradv(phi), gradv(phi) ) )
                            );
                }
                break;

                case FastMarchingInitializationMethod::ILP_L2 :
                {
                    auto const modGradPhi = this->modGrad( phi, DerivationMethod::L2_PROJECTION );
                    //*phiReinit = phi;
                    phiReinit->on( 
                            _range=this->rangeMeshElements(), 
                            _expr=idv(phi)/idv(modGradPhi) 
                            );
                }
                break;

                case FastMarchingInitializationMethod::ILP_SMOOTH :
                {
                    auto const modGradPhi = this->modGrad( phi, DerivationMethod::SMOOTH_PROJECTION );
                    //*phiReinit = phi;
                    phiReinit->on( 
                            _range=this->rangeMeshElements(), 
                            _expr=idv(phi)/idv(modGradPhi) 
                            );
                }
                break;

                case FastMarchingInitializationMethod::HJ_EQ :
                {
                    CHECK(false) << "TODO\n";
                    //*phi = *explicitHJ(max_iter, dtau, tol);
                }
                break;
                case FastMarchingInitializationMethod::IL_HJ_EQ :
                {
                    CHECK(false) << "TODO\n";
                    //*phi = *explicitHJ(max_iter, dtau, tol);
                }
                break;
                case FastMarchingInitializationMethod::NONE :
                {
                    *phiReinit = phi;
                }
                break;
            } // switch M_fastMarchingInitializationMethod

            LOG(INFO)<< "reinit with FMM done"<<std::endl;
            *phiReinit = M_reinitializer->run( *phiReinit );
        } // Fast Marching
        break;

        case LevelSetDistanceMethod::HAMILTONJACOBI:
        {
            // TODO
            *phiReinit = M_reinitializer->run( phi );
        } // Hamilton-Jacobi
        break;

        case LevelSetDistanceMethod::RENORMALISATION:
        {
            //auto R = this->interfaceRectangularFunction( phi );
            auto const modGradPhi = this->modGrad( phi );
            phiReinit->on(
                    _range=this->rangeMeshElements(),
                    //_expr = idv(phi) * ( 1. + ( 1./idv(modGradPhi)-1. )*idv(R) )
                    _expr = idv(phi) / idv(modGradPhi)
                    );
        }
        break;
    }
    
    return *phiReinit;
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
typename LEVELSETBASE_CLASS_TEMPLATE_TYPE::reinitializerFM_ptrtype const&
LEVELSETBASE_CLASS_TEMPLATE_TYPE::reinitializerFM( bool buildOnTheFly )
{
    if( !M_reinitializerFM && buildOnTheFly )
    {
        M_reinitializerFM.reset( 
                new ReinitializerFM<space_levelset_type>( this->functionSpace(), prefixvm(this->prefix(), "reinit-fm") ) 
                );
    }

    return M_reinitializerFM;
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
typename LEVELSETBASE_CLASS_TEMPLATE_TYPE::reinitializerHJ_ptrtype const&
LEVELSETBASE_CLASS_TEMPLATE_TYPE::reinitializerHJ( bool buildOnTheFly )
{
    if( !M_reinitializerHJ && buildOnTheFly )
    {
        M_reinitializerHJ.reset( 
                new ReinitializerHJ<space_levelset_type>( this->functionSpace(), prefixvm(this->prefix(), "reinit-hj") ) 
                );
    }

    return M_reinitializerHJ;
}

//----------------------------------------------------------------------------//
// Initial value
LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
void
LEVELSETBASE_CLASS_TEMPLATE_TYPE::setInitialValue(element_levelset_ptrtype const& phiv, bool doReinitialize)
{
    this->log("LevelSet", "setInitialValue", "start");

    if( !M_initialPhi )
        M_initialPhi.reset( new element_levelset_type(this->functionSpace(), "InitialPhi") );

    if (doReinitialize)
    {
        auto phiRedist = this->redistantiate( *phiv, M_reinitMethod );
        *M_initialPhi = phiRedist;
    }
    else
    {
        *M_initialPhi = *phiv;
    }

    this->log("LevelSet", "setInitialValue", "finish");
}

//----------------------------------------------------------------------------//
//----------------------------------------------------------------------------//
//----------------------------------------------------------------------------//
LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
std::shared_ptr<std::ostringstream>
LEVELSETBASE_CLASS_TEMPLATE_TYPE::getInfo() const
{
    std::string hdProjectionMethod = (this->M_useHeavisideDiracNodalProj)? "nodal": "L2";

    const std::string gradPhiMethod = DerivationMethodMap.right.at(this->M_gradPhiMethod);
    const std::string modGradPhiMethod = DerivationMethodMap.right.at(this->M_modGradPhiMethod);
    const std::string curvatureMethod = CurvatureMethodMap.right.at(this->M_curvatureMethod);

    std::string reinitMethod;
    std::string reinitmethod = soption( _name="reinit-method", _prefix=this->prefix() );
    if( reinitmethod == "fm" )
    {
        reinitMethod = "Fast-Marching";
        std::string fmInitMethod = FastMarchingInitializationMethodIdMap.right.at( this->M_fastMarchingInitializationMethod );
        reinitMethod += " (" + fmInitMethod + ")";
    }
    else if( reinitmethod == "hj" )
        reinitMethod = "Hamilton-Jacobi";

    std::string scalarSmootherParameters;
    if ( M_projectorSMScalar )
    {
        double scalarSmootherCoeff = this->smoother()->epsilon() * Order / this->mesh()->hAverage();
        scalarSmootherParameters = "coeff (h*c/order) = " 
            + std::to_string(this->smoother()->epsilon())
            + " (" + std::to_string(this->mesh()->hAverage()) + " * " + std::to_string(scalarSmootherCoeff) + " / " + std::to_string(Order) + ")"
            ;
    }
    std::string vectorialSmootherParameters;

    if ( M_projectorSMVectorial )
    {
        double vectorialSmootherCoeff = this->smootherVectorial()->epsilon() * Order / this->mesh()->hAverage();
        vectorialSmootherParameters = "coeff (h*c/order) = " 
            + std::to_string(this->smootherVectorial()->epsilon())
            + " (" + std::to_string(this->mesh()->hAverage()) + " * " + std::to_string(vectorialSmootherCoeff) + " / " + std::to_string(Order) + ")"
            ;
    }

    std::string restartMode = (this->doRestart())? "ON": "OFF";

    std::string exporterType = this->M_exporter->type();
    std::string hovisuMode = "OFF";
    int exporterFreq = this->M_exporter->freq();
    std::string exportedFields;
    if ( this->hasPostProcessFieldExported( LevelSetFieldsExported::GradPhi ) )
        exportedFields = (exportedFields.empty())? "GradPhi": exportedFields+" - GradPhi";
    if ( this->hasPostProcessFieldExported( LevelSetFieldsExported::ModGradPhi ) )
        exportedFields = (exportedFields.empty())? "ModGradPhi": exportedFields+" - ModGradPhi";
    if ( this->hasPostProcessFieldExported( LevelSetFieldsExported::Distance ) )
        exportedFields = (exportedFields.empty())? "Distance": exportedFields+" - Distance";
    if ( this->hasPostProcessFieldExported( LevelSetFieldsExported::DistanceNormal ) )
        exportedFields = (exportedFields.empty())? "DistanceNormal": exportedFields+" - DistanceNormal";
    if ( this->hasPostProcessFieldExported( LevelSetFieldsExported::DistanceCurvature ) )
        exportedFields = (exportedFields.empty())? "DistanceCurvature": exportedFields+" - DistanceCurvature";

    std::shared_ptr<std::ostringstream> _ostr( new std::ostringstream() );
    *_ostr << "\n||==============================================||"
           << "\n||---------------Info : LevelSet----------------||"
           << "\n||==============================================||"
           << "\n   Prefix          : " << this->prefix()
           << "\n   Root Repository : " << this->rootRepository()
           << "\n   Dim             : " << nDim
           << "\n   Order           : " << Order
           << "\n   Periodicity     : " << M_periodicity.isPeriodic()

           << "\n   Level Set Parameters"
           << "\n     -- thickness interface (use adaptive)  : " << this->thicknessInterface() << " (" << std::boolalpha << this->M_useAdaptiveThicknessInterface << ")"
           << "\n     -- use regular phi (phi / |grad(phi)|) : " << std::boolalpha << this->M_useRegularPhi
           << "\n     -- Heaviside/Dirac projection method   : " << hdProjectionMethod
           << "\n     -- reinit initial value                : " << std::boolalpha << this->M_reinitInitialValue
           << "\n     -- gradphi projection                  : " << gradPhiMethod
           << "\n     -- modgradphi projection               : " << modGradPhiMethod
           << "\n     -- curvature projection                : " << curvatureMethod

           << "\n   Reinitialization Parameters"
           << "\n     -- reinitialization method         : " << reinitMethod;

    if( M_projectorSMScalar || M_projectorSMVectorial )
    *_ostr << "\n   Smoothers Parameters";
    if( M_projectorSMScalar )
    *_ostr << "\n     -- scalar smoother    : " << scalarSmootherParameters;
    if( M_projectorSMVectorial )
    *_ostr << "\n     -- vectorial smoother : " << vectorialSmootherParameters;

    *_ostr << "\n   Space Discretization";
    if( this->hasGeoFile() )
    *_ostr << "\n     -- geo file name   : " << this->geoFile();
    *_ostr << "\n     -- mesh file name  : " << this->meshFile()
           << "\n     -- nb elt in mesh  : " << this->mesh()->numGlobalElements()//numElements()
         //<< "\n     -- nb elt in mesh  : " << this->mesh()->numElements()
         //<< "\n     -- nb face in mesh : " << this->mesh()->numFaces()
           << "\n     -- hMin            : " << this->mesh()->hMin()
           << "\n     -- hMax            : " << this->mesh()->hMax()
           << "\n     -- hAverage        : " << this->mesh()->hAverage()
           << "\n     -- geometry order  : " << nOrderGeo
           << "\n     -- level set order : " << Order
           << "\n     -- nb dof          : " << this->functionSpace()->nDof() << " (" << this->functionSpace()->nLocalDof() << ")"
           ;

    *_ostr << "\n   Exporter"
           << "\n     -- type            : " << exporterType
           << "\n     -- high order visu : " << hovisuMode
           << "\n     -- freq save       : " << exporterFreq
           << "\n     -- fields exported : " << exportedFields

           << "\n   Processors"
           << "\n     -- number of proc environment : " << Environment::worldComm().globalSize()
           << "\n     -- environment rank           : " << Environment::worldComm().rank()
           << "\n     -- global rank                : " << this->worldComm().globalRank()
           << "\n     -- local rank                 : " << this->worldComm().localRank()
           ;

#if 0
    if ( this->algebraicFactory() )
    *_ostr << this->algebraicFactory()->getInfo()->str();
#endif
    //if (enable_reinit)
    //{
        //if (reinitmethod == "hj")
        //{
            //infos << "\n      * hj maximum iteration per reinit : " << hj_max_iter
                  //<< "\n      * hj pseudo time step dtau : " << hj_dtau
                  //<< "\n      * hj stabilization : SUPG"
                  //<< "\n      * hj coeff stab : " << option( prefixvm(M_prefix,"hj-coeff-stab")).template as<double>()
                  //<< "\n      * hj tolerence on dist to dist error : "<<hj_tol;
        //}
        //else
        //{
            //infos << "\n      * fm smoothing coefficient for ILP : " << Environment::vm()[prefixvm(M_prefix,"fm-smooth-coeff")].template as< double >();
        //}
    //}
    //infos << "\n\n  Level set spaces :"
          //<< "\n     -- scalar LS space ndof : "<< this->functionSpace()->nDof()
          //<< "\n     -- vectorial LS ndof : "<< this->functionSpaceVectorial()->nDof()
          //<< "\n     -- scalar P0 space ndof : "<< this->functionSpaceMarkers()->nDof()
          //<<"\n||==============================================||\n\n";

    *_ostr << "\n||==============================================||"
           << "\n||==============================================||"
           << "\n\n";

    return _ostr;
}

//----------------------------------------------------------------------------//
LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
typename LEVELSETBASE_CLASS_TEMPLATE_TYPE::mesh_ptrtype const&
LEVELSETBASE_CLASS_TEMPLATE_TYPE::submeshDirac() const
{
    if( M_doUpdateSubmeshDirac )
    {
        this->mesh()->updateMarker2( *this->markerDirac() );
        M_submeshDirac = createSubmesh( this->mesh(), marked2elements( this->mesh(), 1 ) );
        M_doUpdateSubmeshDirac = false;
    }
    return M_submeshDirac;
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
typename LEVELSETBASE_CLASS_TEMPLATE_TYPE::mesh_ptrtype const&
LEVELSETBASE_CLASS_TEMPLATE_TYPE::submeshOuter( double cut ) const
{
    if( M_doUpdateSubmeshOuter || cut != M_markerOuterCut )
    {
        this->mesh()->updateMarker2( *this->markerOuter( cut ) );
        M_submeshOuter = createSubmesh( this->mesh(), marked2elements( this->mesh(), 1 ) );
        M_doUpdateSubmeshOuter = false;
    }
    return M_submeshOuter;
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
typename LEVELSETBASE_CLASS_TEMPLATE_TYPE::mesh_ptrtype const&
LEVELSETBASE_CLASS_TEMPLATE_TYPE::submeshInner( double cut ) const
{
    if( M_doUpdateSubmeshInner || cut != M_markerInnerCut )
    {
        this->mesh()->updateMarker2( *this->markerInner( cut ) );
        M_submeshInner = createSubmesh( this->mesh(), marked2elements( this->mesh(), 1 ) );
        M_doUpdateSubmeshInner = false;
    }
    return M_submeshInner;
}

//----------------------------------------------------------------------------//
// Export results
LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
void
LEVELSETBASE_CLASS_TEMPLATE_TYPE::exportResults( double time, bool save )
{
    this->exportResultsImpl( time, save );
    this->exportMeasuresImpl( time, save );
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
bool
LEVELSETBASE_CLASS_TEMPLATE_TYPE::exportResultsImpl( double time, bool save )
{
    this->log("LevelSet","exportResults", "start");
    this->timerTool("PostProcessing").start();

    if ( !this->M_exporter->doExport() ) return false;

    bool hasResultToExport = true;

    this->M_exporter->step( time )->add( prefixvm(this->prefix(),"Phi"),
                                         prefixvm(this->prefix(),prefixvm(this->subPrefix(),"Phi")),
                                         this->phiElt() );
    this->M_exporter->step( time )->add( prefixvm(this->prefix(),"Dirac"),
                                   prefixvm(this->prefix(),prefixvm(this->subPrefix(),"Dirac")),
                                   *this->dirac() );

    this->M_exporter->step( time )->add( prefixvm(this->prefix(),"Heaviside"),
                                   prefixvm(this->prefix(),prefixvm(this->subPrefix(),"Heaviside")),
                                   *this->heaviside() );

    this->M_exporter->step( time )->add( prefixvm(this->prefix(),"Normal"),
                                   prefixvm(this->prefix(),prefixvm(this->subPrefix(),"Normal")),
                                   *this->normal() );

    this->M_exporter->step( time )->add( prefixvm(this->prefix(),"Curvature"),
                                   prefixvm(this->prefix(),prefixvm(this->subPrefix(),"Curvature")),
                                   *this->curvature() );

    if ( this->hasPostProcessFieldExported( LevelSetFieldsExported::GradPhi ) )
    {
        this->M_exporter->step( time )->add( prefixvm(this->prefix(),"GradPhi"),
                                       prefixvm(this->prefix(),prefixvm(this->subPrefix(),"GradPhi")),
                                       *this->gradPhi() );
    }
    if ( this->hasPostProcessFieldExported( LevelSetFieldsExported::ModGradPhi ) )
    {
        this->M_exporter->step( time )->add( prefixvm(this->prefix(),"ModGradPhi"),
                                       prefixvm(this->prefix(),prefixvm(this->subPrefix(),"ModGradPhi")),
                                       *this->modGradPhi() );
    }
    if ( this->hasPostProcessFieldExported( LevelSetFieldsExported::Distance ) )
    {
        this->M_exporter->step( time )->add( prefixvm(this->prefix(),"Distance"),
                                       prefixvm(this->prefix(),prefixvm(this->subPrefix(),"Distance")),
                                       *this->distance() );
    }
    if ( this->hasPostProcessFieldExported( LevelSetFieldsExported::DistanceNormal ) )
    {
        this->M_exporter->step( time )->add( prefixvm(this->prefix(),"DistanceNormal"),
                                       prefixvm(this->prefix(),prefixvm(this->subPrefix(),"DistanceNormal")),
                                       *this->distanceNormal() );
    }
    if ( this->hasPostProcessFieldExported( LevelSetFieldsExported::DistanceCurvature ) )
    {
        this->M_exporter->step( time )->add( prefixvm(this->prefix(),"DistanceCurvature"),
                                       prefixvm(this->prefix(),prefixvm(this->subPrefix(),"DistanceCurvature")),
                                       *this->distanceCurvature() );
    }

    if( save )
        this->M_exporter->save();

    return hasResultToExport;

    double tElapsed = this->timerTool("PostProcessing").stop("exportResults");
    this->log("LevelSet","exportResults", (boost::format("finish in %1% s")%tElapsed).str() );
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
bool
LEVELSETBASE_CLASS_TEMPLATE_TYPE::exportMeasuresImpl( double time, bool save )
{
    bool hasMeasureToExport = false;

    if( this->hasPostProcessMeasureExported( LevelSetMeasuresExported::Volume ) )
    {
        this->postProcessMeasuresIO().setMeasure( "volume", this->volume() );
        hasMeasureToExport = true;
    }
    if( this->hasPostProcessMeasureExported( LevelSetMeasuresExported::Perimeter ) )
    {
        this->postProcessMeasuresIO().setMeasure( "perimeter", this->perimeter() );
        hasMeasureToExport = true;
    }
    if( this->hasPostProcessMeasureExported( LevelSetMeasuresExported::Position_COM ) )
    {
        auto com = this->positionCOM();
        std::vector<double> vecCOM = { com(0,0) };
        if( nDim > 1 ) vecCOM.push_back( com(1,0) );
        if( nDim > 2 ) vecCOM.push_back( com(2,0) );
        this->postProcessMeasuresIO().setMeasureComp( "position_com", vecCOM );
        hasMeasureToExport = true;
    }

    if( save && hasMeasureToExport )
    {
        if ( !this->isStationary() )
            this->postProcessMeasuresIO().setMeasure( "time", time );
        this->postProcessMeasuresIO().exportMeasures();
        this->upload( this->postProcessMeasuresIO().pathFile() );
    }

    return hasMeasureToExport;
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
bool
LEVELSETBASE_CLASS_TEMPLATE_TYPE::hasPostProcessMeasureExported( 
        LevelSetMeasuresExported const& measure) const
{
    return M_postProcessMeasuresExported.find(measure) != M_postProcessMeasuresExported.end();
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
bool
LEVELSETBASE_CLASS_TEMPLATE_TYPE::hasPostProcessFieldExported( 
        LevelSetFieldsExported const& field) const
{
    return M_postProcessFieldsExported.find(field) != M_postProcessFieldsExported.end();
}

//----------------------------------------------------------------------------//
// Physical quantities
LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
double
LEVELSETBASE_CLASS_TEMPLATE_TYPE::volume() const
{
    double volume = integrate(
            _range=this->rangeMeshElements(),
            _expr=(1-idv(this->heaviside())) 
            ).evaluate()(0,0);

    return volume;
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
double
LEVELSETBASE_CLASS_TEMPLATE_TYPE::perimeter() const
{
    double perimeter = integrate(
            _range=this->rangeDiracElements(),
            _expr=this->diracExpr()
            ).evaluate()(0,0);

    return perimeter;
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
auto
LEVELSETBASE_CLASS_TEMPLATE_TYPE::positionCOM() const
{
    auto com = integrate( 
            _range=this->rangeMeshElements(), 
            _expr=vf::P() * (1.-idv(this->H()))
            ).evaluate();
    com = com / this->volume();

    return com;
}

//----------------------------------------------------------------------------//
//----------------------------------------------------------------------------//
//----------------------------------------------------------------------------//
// Update markers
LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
void
LEVELSETBASE_CLASS_TEMPLATE_TYPE::updateMarkerInterface()
{
    /* returns a marker (P0_type) on the elements crossed by the levelset
       ie :
       - if the element has not phi on all the dof of the same sign -> the element is mark as 1
       - if phi on the dof of the element are of the same sign -> mark as 0
      */

    const int ndofv = space_levelset_type::fe_type::nDof;

    mesh_ptrtype const& mesh = this->mesh();
    auto phi = this->phiPtr();

    auto rangeElts = mesh->elementsWithProcessId( mesh->worldComm().localRank() );
    auto it_elt = std::get<0>( rangeElts );
    auto en_elt = std::get<1>( rangeElts );
    if (it_elt == en_elt) return;

    for (; it_elt!=en_elt; it_elt++)
    {
        auto const& elt = boost::unwrap_ref( *it_elt );
        int nbplus = 0;
        int nbminus = 0;

        for (int j=0; j<ndofv ; j++)
        {
            if (phi->localToGlobal(elt.id(), j, 0) >= 0.)
                nbplus++;
            else
                nbminus++;
        }

        //if elt crossed by interface
        if ( (nbminus != ndofv) && (nbplus!=ndofv) )
            M_markerInterface->assign(elt.id(), 0, 0, 1);
        else
            M_markerInterface->assign(elt.id(), 0, 0, 0);
    }
}

LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
void
LEVELSETBASE_CLASS_TEMPLATE_TYPE::updateMarkerDirac()
{
    const int ndofv = space_levelset_type::fe_type::nDof;

    mesh_ptrtype const& mesh = this->mesh();

    auto rangeElts = mesh->elementsWithProcessId( mesh->worldComm().localRank() );
    auto it_elt = std::get<0>( rangeElts );
    auto en_elt = std::get<1>( rangeElts );
    if (it_elt == en_elt) return;

    double dirac_cut = this->dirac()->max() / 10.;

    for (; it_elt!=en_elt; it_elt++)
    {
        auto const& elt = boost::unwrap_ref( *it_elt );
        bool mark_elt = false;
        for (int j=0; j<ndofv; j++)
        {
            if ( std::abs( this->dirac()->localToGlobal(elt.id(), j, 0) ) > dirac_cut )
            {
                mark_elt = true;
                break; //don't need to do the others dof
            }
        }
        if( mark_elt )
            M_markerDirac->assign(elt.id(), 0, 0, 1);
        else
            M_markerDirac->assign(elt.id(), 0, 0, 0);
    }
}//markerDirac


LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
void
LEVELSETBASE_CLASS_TEMPLATE_TYPE::markerHeavisideImpl( element_markers_ptrtype const& marker, bool invert, double cut )
{
    /* returns P0 element having :
    if invert == true : 1 on elements inside Heaviside function (where H is smaller than epsilon on at least 1 dof)
    if invert == false : 1 on elements where Heaviside function greater than epsilon
    if cut_in_out == true : marker = 1 where H>0.5 and 0 where H<0.5
    */

    const int ndofv = space_levelset_type::fe_type::nDof;

    mesh_ptrtype const& mesh = this->mesh();

    auto rangeElts = mesh->elementsWithProcessId( mesh->worldComm().localRank() );
    auto it_elt = std::get<0>( rangeElts );
    auto en_elt = std::get<1>( rangeElts );
    if (it_elt == en_elt) return;

    for (; it_elt!=en_elt; it_elt++)
    {
        auto const& elt = boost::unwrap_ref( *it_elt );
        bool mark_elt = false;
        for (int j=0; j<ndofv; j++)
        {
            if ( std::abs( this->heaviside()->localToGlobal(elt.id(), j, 0) ) > cut )
            {
                mark_elt = true;
                break;
            }
        }
        if( mark_elt )
            marker->assign(elt.id(), 0, 0, (invert)?0:1);
        else
            marker->assign(elt.id(), 0, 0, (invert)?1:0);
    }
} 

//LEVELSETBASE_CLASS_TEMPLATE_DECLARATIONS
//void
//LEVELSETBASE_CLASS_TEMPLATE_TYPE::updateMarkerCrossedElements()
//{
    //// return a "marker" on the elements traversed by the interface between phio and phi
    //// ie : mark as 1 is one of the dof of the element has  (phi * phio < 0)
    //const int ndofv = space_levelset_type::fe_type::nDof;

    //mesh_ptrtype const& mesh = this->mesh();

    //auto rangeElts = mesh->elementsWithProcessId( mesh->worldComm().localRank() );
    //auto it_elt = std::get<0>( rangeElts );
    //auto en_elt = std::get<1>( rangeElts );
    //if (it_elt == en_elt) return;

    //auto phi = this->phiPtr();
    //auto phio = this->phio();

    //auto prod = vf::project(this->functionSpace(), this->rangeMeshElements(),
                            //idv(phio) * idv(phi) );


    //for (; it_elt!=en_elt; it_elt++)
    //{
        //auto const& elt = boost::unwrap_ref( *it_elt );
        //bool mark_elt = false;
        //for (int j=0; j<ndofv ; j++)
        //{
            //if (prod.localToGlobal(elt.id(), j, 0) <= 0.)
            //{
                //mark_elt = true;
                //break;
            //}
        //}
        //if( mark_elt )
            //M_markerCrossedElements->assign(elt.id(), 0, 0, 1);
        //else
            //M_markerCrossedElements->assign(elt.id(), 0, 0, 0);
    //}
//}

} // FeelModels
} // Feel