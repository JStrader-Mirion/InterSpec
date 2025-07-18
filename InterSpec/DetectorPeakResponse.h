#ifndef DetectorPeakResponse_h
#define DetectorPeakResponse_h
/* InterSpec: an application to analyze spectral gamma radiation data.
 
 Copyright 2018 National Technology & Engineering Solutions of Sandia, LLC
 (NTESS). Under the terms of Contract DE-NA0003525 with NTESS, the U.S.
 Government retains certain rights in this software.
 For questions contact William Johnson via email at wcjohns@sandia.gov, or
 alternative emails of interspec@sandia.gov.
 
 This library is free software; you can redistribute it and/or
 modify it under the terms of the GNU Lesser General Public
 License as published by the Free Software Foundation; either
 version 2.1 of the License, or (at your option) any later version.
 
 This library is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 Lesser General Public License for more details.
 
 You should have received a copy of the GNU Lesser General Public
 License along with this library; if not, write to the Free Software
 Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "InterSpec_config.h"

#include <deque>
#include <cmath>
#include <tuple>
#include <thread>
#include <memory>
#include <cctype>
#include <string>
#include <vector>
#include <cstring>
#include <istream>
#include <sstream>
#include <utility>
#include <sstream>
#include <stdexcept>
#include <functional>

#include <Wt/Dbo/Dbo>
#include <Wt/WDateTime>
#include <Wt/Dbo/SqlTraits>
#include <Wt/Dbo/WtSqlTraits>

#include "InterSpec/PeakModel.h"
#include "SpecUtils/StringAlgo.h"
#include "InterSpec/InterSpecApp.h"
#include "InterSpec/PhysicalUnits.h"
#include "InterSpec/WarningWidget.h"


class PeakDef;
class PeakModel;
class InterSpecUser;
namespace SpecUtils{ class Measurement; }

namespace rapidxml
{
  template<class Ch> class xml_node;
  template<class Ch> class xml_document;
}//namespace rapidxml

/** TODO:
 - Add a "setback" option
    - There are a couple commented-out functions where this is started, but will need to
      - Add `m_setback` member variable
      - Modify the "DetectorPeakResponse" DB class - need to use DB upgrade mechanism
      - Make sure that everywhere that gets a fractional solid angle, accounts for the setback.
      - At the same time, add a "fixed geometry" option, where distances then disappear everywhere
 - Add ability to store original intrinsic efficiencies, and their uncertainties, and similarly for FWHM.
 - Add ability to add "additional" energy dependent uncertainties
  - Need to think about how to handle correlations between energies
 - Add material type and depth, so can approximate any interaction for cascade summing corrections
 */


//Forward declarations
struct FormulaWrapper;


class DetectorPeakResponse
{
  /*
   //An example of defining a detectors efficiency via functional form:
   DetectorPeakResponse det( "DetectorName", "Detector Description" );
   
   const std::string fcn = "exp(-343.6330974237 + 269.1023287277*log(x)"
                           "+ -83.8077567526*log(x)^2  + 12.9980559362*log(x)^3"
                           "+ -1.0068649823*log(x)^4 + 0.0311640084*log(x)^5)";
   try
   {
     float detector_diameter = PhysicalUnits::stringToDistance( "2.2 cm" );
     det.setIntrinsicEfficiencyFormula( fcn, detector_diameter, 1.0f, 0.0f, 0.0f );
   }catch( std::exception &e )
   {
     std::cerr << "Caught: " << e.what() << std::endl;
     return;
   }//try / catch
   
   std::cout << det.intrinsicEfficiency(121.78f) << std::endl; //0.625191
   std::cout << det.intrinsicEfficiency(411.02f) << std::endl; //0.333307
   std::cout << det.intrinsicEfficiency(500.0f) << std::endl;    //0.285503
   std::cout << det.intrinsicEfficiency(700.0f) << std::endl;    //0.219004
   std::cout << det.intrinsicEfficiency(800.0f) << std::endl;    //0.197117
  */
  
public:
  enum ResolutionFnctForm
  {
    kGadrasResolutionFcn, //See peakResolutionFWHM() implementation
    
    /**  
     FWHM = sqrt( Sum_i{A_i*pow(x/1000,i)} );
     */
    kSqrtPolynomial,  //
    
    /**
     FWHM = `sqrt(A0 + A1*E + A2/E)`
     */
    kSqrtEnergyPlusInverse,
    
    /**
     FWHM = `A0 + A1*sqrt(E)`
     
     For use with other applications - do not recommend actually using.
     */
    kConstantPlusSqrtEnergy,
    
    kNumResolutionFnctForm
  };//enum ResolutionFnctForm

  
  enum EfficiencyFnctForm
  {
    kEnergyEfficiencyPairs,
    kFunctialEfficienyForm,
    kExpOfLogPowerSeries,
    kNumEfficiencyFnctForms
  };//enum EfficiencyFnctForm
  
  
  /** How the efficiency should be interpreted.
   */
  enum class EffGeometryType
  {
    /** This is the default DRF type, and allows the detection efficiency to vary with ~1/r2.
     */
    FarField            = 0x00,
    
    /** This describes where the transport from the source to the detector has already been performed,
     and the full-energy efficiency, per source decay, is known.  I.e. allows you to fit for the total activity of
     the source.
     */
    FixedGeomTotalAct   = 0x01,
    
    /** Similar to #FixedGeomTotalAct, but instead the efficiency describes full-energy efficiency,
     per cm2 surface area of source.  I.e., allows you to fit for contamination per cm2.
     */
    FixedGeomActPerCm2  = 0x02,
    
    /** Similar to #FixedGeomTotalAct, but instead the efficiency describes full-energy efficiency,
     per m2 surface area of source.  I.e., allows you to fit for contamination per m2.
     */
    FixedGeomActPerM2   = 0x04,
    
    /** Similar to #FixedGeomTotalAct, but instead the efficiency describes full-energy efficiency,
     per gram of source.  I.e., allows you to fit for contamination per gram.
     */
    FixedGeomActPerGram = 0x08
  };//enum class EffGeometryType
  
  /** Returns the postfix to add to an activity value, for the type of geometry the DRF is for.
   
   For far-field and fixed-total-act, returns empty string.
   Otherwise, returns "/cm2", "/m2", or "/g", as appropriate.
   */
  static const std::string &det_eff_geom_type_postfix( const EffGeometryType type );
  
  
  /** Enum used to indicate where the DRF came from.  This is used primarily to
      help decide what DRFs to show when user browses database.
   */
  enum DrfSource
  {
    UnknownDrfSource = 4,
    
    /** One of the GADRAS based DRFs that come with InterSpec. */
    DefaultGadrasDrf = 0,
    
    /** A GADRAS DRF from the filesystem (like C:\Gadras\Detectors, or
     InterSpecs user data directory.
     */
    UserAddedGadrasDrf = 5,
    
    /** Relative (or intrinsic) efficiency DRF from a CSV or TSV from a user
     specified directory, like InterSpecs user data directory.
     */
    UserAddedRelativeEfficiencyDrf = 1,
    
    /** User used the "Import" of Detector Select tool to upload Efficiency.csv
     file, and specified the detector diameter.
    */
    UserImportedIntrisicEfficiencyDrf = 2,
    
    /** User used the "Import" of DetectorSelect tool to upload an
        Efficiency.csv and Detector.dat file.
     */
    UserImportedGadrasDrf = 6,
    
    /** User specified a formula in the Detector Select Tool. */
    UserSpecifiedFormulaDrf = 3,
    
    /** User used the MakeDrf tool to create the DRF. */
    UserCreatedDrf = 7,
    
    /** The DRF was included in a spectrum (N42) file. */
    FromSpectrumFileDrf = 8,
    
    /** Relative (or intrinsic) efficiency DRF from a CSV or TSV from InterSpecs static "data"
     directory.
     */
    DefaultRelativeEfficiencyDrf = 9,
    
    /** From ISOCS .ECC file. */
    IsocsEcc = 10,
  };//enum DrfSource
  
public:
  //DetectorPeakResponse(): constructs with no defined resolution, efficiency,
  //  or description; detector name is "DetectorPeakResponse".
  DetectorPeakResponse();
  
  
  //DetectorPeakResponse(...): constructs with no defined resolution or
  //  efficiency, but with given name and description.
  DetectorPeakResponse( const std::string &name,
                        const std::string &descrip = "" );
  
  
  
  //Destructor
  virtual ~DetectorPeakResponse();
  
  //The hash value gives a fast/unique way to compare detectors.
  uint64_t hashValue() const;
  uint64_t parentHashValue() const;
  void setParentHashValue( const uint64_t val );

  //isValid(): Tells wether or not this DetectorPeakResponse has been properly
  //  defined and can be used.  Right now this is solely based on whether or
  //  not 'm_energyEfficiencies' has been populated.
  bool isValid() const;
  
  
  //hasResolutionInfo(): Tells whether or not this DetectorPeakResponse has
  //  (valid) detector resolution information
  bool hasResolutionInfo() const;
  

  //reset(): clears all data, and causes isValid() to return false
  void reset();


  //operator==: compares all member varialbles.  Currently doesnt account for
  //  small precision errors on floating point variables.
  bool operator==( const DetectorPeakResponse &rhs ) const;


  //fromEnergyEfficiencyCsv(...): first field of each line should be centroid,
  //  and second the absolute efficiency (so efficiency of gammas hitting
  //  detector face) in percents, of the detector.
  //  All lines with non-digit first non-whitespace characters, will be ignored,
  //  as will lines with only one number on them.
  //  Input can be space, comma, or tab separated.
  //  Will throw std::runtime_error() on input error.
  //  Detector diameter is in units of PhysicalUnits
  //  Recomputes hash value.
  void fromEnergyEfficiencyCsv( std::istream &input,
                               const float detectorDiameter,
                               const float energyUnits,
                               const EffGeometryType geometry_type );
  
  
  //setIntrinsicEfficiencyFormula(): sets m_efficiencyForm, m_efficiencyFormula,
  //  and m_efficiencyFcn to correspond to the fucntional form passed in.
  //  Formula should be in the a general functional form, where energy is
  //  represented by the letter 'x' and is in keV.  The formula is for the
  //  intrinsic efficiency of the detector, and should range from 0.0 to 1.0,
  //  in the valid energy range.
  //  Recomputes hash value.
  //e.x.
  //  fcn = "exp(-343.6330974237 + 269.1023287277*log(x)"
  //        "+ -83.8077567526*log(x)^2  + 12.9980559362*log(x)^3"
  //        "+ -1.0068649823*log(x)^4 + 0.0311640084*log(x)^5)";
  //Should give:
  //  intrinsicEfficiency(121.78)==0.625191;
  //  intrinsicEfficiency(411.02)==0.333307;
  //  intrinsicEfficiency(500)   ==0.285503;
  //  intrinsicEfficiency(700)   ==0.219004;
  //  intrinsicEfficiency(800)   ==0.197117
  //
  //The 'detector_diameter' is in units of PhysicalUnits (e.g. mm), and
  //  will not be used if 'fixedGeometry' is true..
  //
  //The lower and upper energy parameters specify the energy range the function
  //  is valid over; if unknown specify 0.0f for both.
  //
  // The 'fixedGeometry' is If this efficiency is for a fixed geometry
  //  (e.g. Marinelli beaker, soil contamination, etc).
  //
  //Throws std::runtime_error if invalid expression, with a message that is
  //  valid HTML.
  //Note, valid function names are:
  //   abs min, max, sqrt, pow, sin, cos, tan, asin, acos, atan, atan2,
  //    sinh, cosh, tanh, exp, log, ln (synonym for log), log10
  void setIntrinsicEfficiencyFormula( const std::string &fcn,
                                      const float detector_diameter,
                                      const float eqnEnergyUnits,
                                      const float lowerEnergy,
                                      const float upperEnergy,
                                      const EffGeometryType geometry_type );
  
  /** Makes a callable function from the provided mathematical formula
   
   @param fcn Mathematical formula to evaluate.  Example: "exp( 1.2 + 3.2*ln(x) + -2.1*ln(x)^2 )"
   @param isMeV Whether units are in MeV, or keV
   
   Throws exception if formula is invalid.
   */
  static std::function<float(float)> makeEfficiencyFunctionFromFormula( const std::string &formula,
                                                                       const bool isMeV );
  
  //fromGadrasDefinition(...): accepts the Efficiency.csv and Detector.dat
  //  files from GADRAS to define the detector.
  //Note that just the Detector.dat file contains enough info to define the
  //  detector, so I could try to only accept this one file, but this isnt
  //  too important right now, since Efficiency.csv is also provided.
  //  Recomputes hash value.
  void fromGadrasDefinition( std::istream &efficiencyCsvFile,
                             std::istream &detDatFile );

  /** Convience function that calls #fromGadrasDefinition with the
      Efficiency.csv and Detector.dat files in the specified directory.
      Throws exception on issue.
   */
  void fromGadrasDirectory( const std::string &dir );
  
  
  /** Sets the detectors as a kExpOfLogPowerSeries effiency detector, using the
      fit absolute efficiency coefficients.
   
   \param coefs The coefficients for the exp(A0 + A1*log(x) + A2*log(x)^2...)
          equation.  If coefficients are not for intrinsic efficiency (e.g.,
          characterizationDist is not 0.0), they coefficients will be converted
          to intrinsic for internal use
   \param uncerts The uncertainties of the coefficients; must either be empty,
          or the same size as \p coefs.
   \param characterizationDist Distance used when fitting coefficients.  If
          coefficients are for intrinsic efficiency, or for fixed geometry, then this
          value will be 0.0f.
   \param equationEnergyUnits The energy units equation should be evaluated
          in. If eqn is in MeV, this will be 1000.  If in keV, will be 1.0.
   \param lowerEnergy Lower energy, in keV, the equation is good for; if
          unknown pass a value of 0.0f for this parameter and upperEnergy.
   \param upperEnergy Upper energy, in keV, the equation is good for; if
          unknown pass a value of 0.0f for this parameter and lowerEnergy.
   \param geometry_type The geometry type the efficiency is for.
   
   Recomputes hash value.
  */
  void fromExpOfLogPowerSeriesAbsEff( const std::vector<float> &coefs,
                                      const std::vector<float> &uncerts,
                                      const float characterizationDist,
                                      const float detector_diameter,
                                      const float equationEnergyUnits,
                                      const float lowerEnergy,
                                      const float upperEnergy,
                                      const EffGeometryType geometry_type );

  
  static std::shared_ptr<DetectorPeakResponse> parseSingleCsvLineRelEffDrf( std::string &line );
  
  /** Parses CSV/TSV files that contain one or more Relative Efficiency DRFs, where each each line specifies a DRF via equation
   coefficients, or a DRF can be specified using its "app-url" encoded representation when
   there are four fields to the line and the third field equals "UrlEncoded", and the fourth field
   contains the data portion of URL (i.e., the part of the URL that comes after the '?' character in
   a deep-link).
   
   Detector Name, Relative Eff, Detector Description,  c0,  c1,  c2,  c3,  c4,  c5,  c6,  c7,  p0,  p1,  p2,  Calib Distance, Radius (cm),  G factor
   Detector Name: is usually the detector model, ex., "IdentiFINDER".
   Relative Eff:. is the efficiency relative to a 3x3 NaI detector at 661 keV; this is only used for reference, and may be empty, ex., "11%"
   Detector Description: Description of the detector crystal, ex., "2.5x1.5 Planar HPGe"
   c0 through c7: the coefficients that will be fed into #fromExpOfLogPowerSeriesAbsEff and are in MeV (not keV); it is not uncommon for
              only the first 4 or five coefficients to be non-zero.  These coefficients are teh relative efficiency at a given distance.
   p0 through p2: unused. interaction depth coefficients
   Calib Distance: the distance, in cm, the relative efficiency coefficients (c0 through c7) are defined for; a value of zero means the
              coefficients are for the intrinsic efficiency (i.e., efficiency of gamma on detector face to be recorded at full-energy)
   Radius (cm): Radius of the detector face.
   G factor: unused. The geometric factor (i.e., fraction solid angle of the detector) at the calibration distance.
   
   An example input line might be:
   <pre>
   My Det, 100%, 3x3 NaI, -9.20, -0.66, -0.096, 0.0009, 0, 0, 0, 0, 0, 0, 0, 100, 3.81, 5.6E-05
   </pre>
   
   Lines starting with a '#' character are be ignored, unless it is a '#credit' line.
   
   @param input The input CSV or TSV file stream
   @param credits Lines in the file starting with '#credit:' will be added into this vector
   @param drfs The DRFs parsed from the file.  If empty after return, not DRFs could be parsed.
   */
  static void parseMultipleRelEffDrfCsv( std::istream &input,
                                        std::vector<std::string> &credits,
                                        std::vector<std::shared_ptr<DetectorPeakResponse>> &drfs );
  
  /** Parses detector efficiency function originating from GammaQuant.
   These detector efficiency functions are defined in a column format.
   A major limitation is that no CSV cell may have a new-line in it.
   
   Throws std::exception on format error, or now det eff functions found
   */
  static void parseGammaQuantRelEffDrfCsv( std::istream &input,
                                          std::vector<std::shared_ptr<DetectorPeakResponse>> &drfs,
                                          std::vector<std::string> &credits,
                                          std::vector<std::string> &warnings );
  
  
  /** Creates a DetectorPeakResponse from "App URL" data.
   Takes in just the "query" portion of the URL (i.e., the data after the '?' character), that
   comes from, say, a QR code.
   
   String passed ins is assumed to have already been url-decoded.
   
   Throws exception on failure, otherwise returns a valid DRF.
   */
  static std::shared_ptr<DetectorPeakResponse> parseFromAppUrl( const std::string &url_query );
  
  /** Converts this DRF to a string that can be used as the "query" portion of a URL.
   
   This function will try to fit the entire DRF within the limits imposed by a QR code, but
   if that isnt possible, it will then try the following until things fit:
   - Remove: last used, DrfSource, parents hash, created data, energy range, hash, description,
             uncertainties, FWHM info, and finally name
   - Admit failure and throw an exception.
   
   In the future it may be implemented that the description is only cut down as much as needed,
   or all characters converted to QR-ascii so the available number of characters is larger.
   
   The returned string is url-encoded - unlike the `toAppUrl()` function of other classes; this
   is to allow the returned string to be represented as a ASCII-mode QR code.
   
   If this DRF is not valid, will throw an exception.
   */
  std::string toAppUrl() const;
  
  /** Decodes the "query" portion of a URL to form the DRF.
   
   String passed in is assumed to have already been url decoded.
   
   If URL is not a valid DRF, throws exception.
   */
  void fromAppUrl( std::string url_query );
  
  /** Parses a .ECC file from ISOCS into a fixed-geometry DRF.
   
   On failure, will throw exception.
   
   Returns a tuple containing:
    - a valid DRF with (efficiencyFcnType() == kEnergyEfficiencyPairs)
    - Source surface area, in units of PhysicalUnits; a zero value will be given if invalid
    - Source mass, in units of PhysicalUnits; a zero value will be given if invalid
   */
  static std::tuple<std::shared_ptr<DetectorPeakResponse>,double,double>
                                              parseEccFile( std::istream &input );
  
  /** Converts a fixed geometry, total activity, DRF to a far-field measurement.
   
   @param diameter The detector diameter.
   @param distance The distance the current efficiency is at.
   @param correct_for_air_atten Wether air attenuation, for #distance, should be backed out.
          Only valid if this DRF is from energy-efficiency pairs (e.g., from CSV or .ECC), or functional efficiency form.
   @returns The new detector response function.
   
   Throws exception if:
   - this DRF is not a `EffGeometryType::FixedGeomTotalAct`
   - If air attenuation is asked to be corrected for, but `efficiencyFcnType() == kExpOfLogPowerSeries`.
   - this DRF is not valid, or diameter is zero or less, or distance is less than zero.
   */
  std::shared_ptr<DetectorPeakResponse> convertFixedGeometryToFarField( const double diameter,
                                                          const double distance,
                                                          const bool correct_for_air_atten ) const;
  
  /** Converts between the fixed geometry types of EffGeometryType.
   
   @param quantity Either the surface area or mass (in units of PhysicalUnits), depending on value to `to_type`.
          Will throw exception if value is less than or equal to zero.
   @param to_type The fixed geometry type to convert to.  Will throw exception if `EffGeometryType::FarField`.
   
   Throws exception `this->m_geomType` or `to_type` is `EffGeometryType::FarField`.
   */
  std::shared_ptr<DetectorPeakResponse> convertFixedGeometryType( const double quantity,
                                                             const EffGeometryType to_type ) const;
  
  /**
   if form==kGadrasResolutionFcn then coefs must have 3 entries
   if form==kSqrtPolynomial then coefs must not be empty,
      and coefficients must have been fit for energy in MeV
   if form==kNumResolutionFnctForm then coefs must be empty
   */
  void setFwhmCoefficients( const std::vector<float> &coefs,
                            const ResolutionFnctForm form );
  
  /** Returns efficiency of a full energy detection event, per decay measured at `distance`.
   
   Energy and distance should be in units of SandiaDecay (e.g. keV=1.0).
   
   If a fixed-geometry DRF, then distance must either be zero or negative, in which case will
   just return the same thing as #intrinsicEfficiency
   
   Will throw `std::runtime_exception` if this object has not been initialized.
   Above or below maximum energies of the efficiency will return upper or lower efficiencies, respectively.
   */
  double efficiency( const float energy, const double distance ) const;


  /** Returns the fraction of gamma rays, at the specified energy, striking the face of the detector,
   will result in a full-energy detection event.  Or for fixed-geometry efficiencies, returns the efficiency
   of a gamma to be detected, per bq of the source (or similar per unit area, if for a surface distribution).
   
   Will throw `std::runtime_exception` if this object has not been initialized.
   Above or below maximum energies of the efficiency will return upper or lower efficiencies, respectively.
   */
  float intrinsicEfficiency( const float energy ) const;

  /** Returns a std::function that gives intrinsic efficiency as a function
  of energy.  Useful primarily for places when you don't want to have this
  class as a dependancy.
  Returns null function if not available.
  */
  std::function<float( float )> intrinsicEfficiencyFcn() const;

  /** Gives the fraction of gammas or x-rays from a point source that would strike the detector crystal.
   
   @param detector_diameter The diameter of the detector, in units of PhysicalUnits.
   @param observation_distance The distance from the face of the detector, to the point source.
          if the detector has a setback, you should add this to the distance of the detector face to
          the point source
   
   Note: For a detector diameter of 5cm, you might start running into numerical accuracy
   issues for distances around 100 km.
   */
  static double fractionalSolidAngle( const double detector_diameter,
                                     const double observation_distance ) noexcept;

  /** Returns approximate fraction of gammas or x-rays from a extended disk-source, that would strike
   the detector crystal.
 
   @param detector_diameter The diameter of the detector, in units of PhysicalUnits.
   @param observation_distance The distance from the face of the detector, to the point source.
          if the detector has a setback, you should add this to the distance of the detector face to
          the point source
   @param source_radius The radius of the flat, round plane, source, in units of PhysicalUnits.
   
   Note: Source is assumed to be flat round plane.
   Note: see pg 119 in Knoll for details on approximation used.
   */
  static double fractionalSolidAngle( const double detector_diameter,
                                     const double observation_distance,
                                     const double source_radius );


  //peakResolutionFWHM(...): returns the full width at half max of the detector.
  //  If the m_resolutionCoeffs are not defined, an exception is thrown.
  float peakResolutionFWHM( const float energy ) const;
  static float peakResolutionFWHM( float energy,
                                   ResolutionFnctForm fcnFrm,
                                   const std::vector<float> &pars );

  
  //peakResolutionSigma(...): returns the resolution sigma of the detector,
  //  that is FWHM/2.35482.
  //  If the m_resolutionCoeffs are not defined, an exception is thrown.
  float peakResolutionSigma( const float energy ) const;
  static float peakResolutionSigma( const float energy,
                                    ResolutionFnctForm fcnFrm,
                                    const std::vector<float> &pars );

  
  //Simple accessors
  float detectorDiameter() const;
  const std::string &efficiencyFormula() const;
  const std::string &name() const;
  const std::string &description() const;
  DrfSource drfSource() const;
  
  float efficiencyEnergyUnits() const;
  
  //Simple setters (all recompute hash value)
  void setName( const std::string &name );
  void setDescription( const std::string &descrip );

  //Search for: setFwhmCoefficients, fromGadrasDirectory, fromGadrasDefinition, setIntrinsicEfficiencyFormula, fromEnergyEfficiencyCsv, fromExpOfLogPowerSeriesAbsEff
  //  And maybe consider making them take a DrfSource argument
  void setDrfSource( const DrfSource source );
  
  /** Sets the energy range the DRF is valid over; currently not enforced or
      indicated anywhere in InterSpec, but may be in the future.
   */
  void setEnergyRange( const double lower, const double upper );
  
  /** The upper energy (in keV) the DRF is valid to.
   
   By default, if not known, this and upperEnergy will be 0.0.
   
   Note: this limit is not currently enforced/used anywhere in InterSpec.
   */
  double lowerEnergy() const;
  
  /** The upper energy (in keV) the DRF is good to.
   
   By default, if not known, this and lowerEnergy will be 0.0.
   
   Note: this limit is not currently enforced/used anywhere in InterSpec.
   */
  double upperEnergy() const;
  
  /** Returns if this DRF is for a fixed geometry; i.e., not #EffGeometryType::FarField.
    */
  bool isFixedGeometry() const;
  
  /** Returns geometry type that the efficiency curve represents. */
  EffGeometryType geometryType() const;
  
  /** The distance the detector crystal is setback from the face of the detector.
   
   Must be zero, or a positive number.
   */
  //void setDetectorSetback( const double distance );
  
  /** The setback distance of the detector.
   Distances are usually given from the face of the detector, to the item of interest,
   however, there is typically a small distance between the face of the detector, and
   the detection element surface - this is the setback distance.
   */
  //double detectorSetback() const;
  
  /** Updated the #m_lastUsedUtc member variable to current time.  Does not
      save to database.
   */
  void updateLastUsedTimeToNow();
  
  //Some temporary accessors for debugging 2019050
  ResolutionFnctForm resolutionFcnType() const { return m_resolutionForm; }
  const std::vector<float> &resolutionFcnCoefficients() const { return m_resolutionCoeffs; }
  EfficiencyFnctForm efficiencyFcnType() const { return m_efficiencyForm; }
  //std::vector<EnergyEfficiencyPair> m_energyEfficiencies;
  //std::string m_efficiencyFormula;
  //std::function<float(float)> m_efficiencyFcn;
  const std::vector<float> &efficiencyExpOfLogsCoeffs() const { return m_expOfLogPowerSeriesCoeffs; }
  
  
  
  //Some methods to fit detector resolution from data.
  //    Only kinda tested as of 20130428
  typedef std::shared_ptr<const std::deque< std::shared_ptr<const PeakDef> > > PeakInput_t;
  
  
  //fitResolution(...): performs a fit to the passed in peaks, and assigns the
  //  found coeffficients to this DetectorPeakResponse.  Note that this funtion
  //  may iterately discard outlier peaks, who contribute more than 2.5 times
  //  the mean per-peak contribution, down to a minum of 80% of previous peaks,
  //  or a minum of 5 peaks; currently a max of one iteration will be performed,
  //  but this may be changed in the future.
  //  The provided Measurement should be same one used to fit the peaks for.
  //Re-computes hash as well.
  //Throws exception on error or failure.
  void fitResolution( PeakInput_t peaks,
                      const std::shared_ptr<const SpecUtils::Measurement> meas,
                      const ResolutionFnctForm fnctnlForm );
  
  //expOfLogPowerSeriesEfficiency(...): evalutaes absolute efficiency for
  // coefficients of the form:
  //  x=log(energy);  //natural log of energy in keV
  //  abs_eff = exp(coefs[0]+coefs[1]*x+coefs[2]*x^2+coefs[3]*x^3+coefs[4]*x^4+...)
  //It is expected energy is in the same units (e.g. keV, MeV, etc) as the coefs
  static float expOfLogPowerSeriesEfficiency( const float energy,
                                              const std::vector<float> &coefs );
  
  
  void toXml( ::rapidxml::xml_node<char> *parent, 
              ::rapidxml::xml_document<char> *doc ) const;
  void fromXml( const ::rapidxml::xml_node<char> *parent );
  
#if( PERFORM_DEVELOPER_CHECKS )
  //equalEnough(...): tests whether the passed in Measurement objects are
  //  equal, for most intents and purposes.  Allows some small numerical
  //  rounding to occur.
  //Throws an std::exception with a brief explanaition when an issue is found.
  static void equalEnough( const DetectorPeakResponse &lhs,
                           const DetectorPeakResponse &rhs );
#endif

public:
  struct EnergyEfficiencyPair
  {
    float energy;
    float efficiency;
    bool operator<( const EnergyEfficiencyPair &rhs ) const
    { return energy < rhs.energy; }
    bool operator<( const float rhs ) const
    { return energy < rhs; }
    bool operator==( const EnergyEfficiencyPair &rhs ) const
    { return energy==rhs.energy && efficiency==rhs.efficiency; }
  };//struct EnergyEfficiencyPair

  //Returns the energy efficiency pairs
  const std::vector<EnergyEfficiencyPair> &getEnergyEfficiencyPair() const;

  static float akimaInterpolate( const float energy,
                                const std::vector<EnergyEfficiencyPair> &xy );
  
  //20190525: Why is m_user a raw index, and not a Wt::Dbo::ptr<InterSpecUser>?
  //          Maybe for database upgrade so Wt::Dbo doesnt need a reference to
  //          InterSpecUser?
  int m_user;
  
protected:
  void computeHash();
  
protected:
  //intrinsicEfficiencyFrom...(...) functions assume energy is input in keV
  float intrinsicEfficiencyFromPairs( float energy ) const;
  float intrinsicEfficiencyFromFcn( float energy ) const;
  float intrinsicEfficiencyFromExpLnEqn( float energy ) const;
  
  std::string m_name;
  std::string m_description;

  //m_detectorDiameter: assumes a round detector face, if other shape you will
  //  need to find the equivalent diameter for the face surface area of
  //  detector
  float m_detectorDiameter;

  //m_efficiencyEnergyUnits: units the absolute energy efficiency formula,
  //  equation, or EnergyEfficiencyPairs are expecting.  Defaults to
  //  PhysicalUnits::keV
  float m_efficiencyEnergyUnits;

  ResolutionFnctForm m_resolutionForm;
  std::vector<float> m_resolutionCoeffs;
  /** Valid only if same size as m_resolutionCoeffs. */
  std::vector<float> m_resolutionUncerts;
  
  DrfSource m_efficiencySource;
  
  //
  EfficiencyFnctForm m_efficiencyForm;
  
  //Design decision: I dont like have member variables that are only used for
  //  certain m_efficiencyForm types, and would have preferred to just have a
  //  single std::function<double(double)> object that would abstract away
  //  the differences, however, the ability to serialize the detector response
  //  function easily, made this difficult to accomplish, so I'm doing it the
  //  bone-headed way I'm not entirely satisfied with at the moment.
  
  //m_energyEfficiencies: the raw intrinsic energy to efficiency pairs, only
  //  filled out if (m_efficiencyForm==kEnergyEfficiencyPairs), which also
  //  implies the efficiency came from a CSV or ECC file.
  std::vector<EnergyEfficiencyPair> m_energyEfficiencies;
  
   
  //m_efficiencyFormula: the raw functional form for the intrinsic effeiciency,
  // e.x. "-343.6 + 269.1*ln(x) + -83.8*ln(x)^2  + 13.0*ln(x)^3".
  //  Only filled out if m_efficiencyForm==kFunctialEfficienyForm.
  // 'x' should is energy in keV, and function should vary between 0.0 and 1.0
  //  for valid energy range.
  std::string m_efficiencyFormula;
  
  //m_efficiencyFcn: the actual function that returns the intrinsic efficiency
  //  when m_efficiencyForm==kFunctialEfficienyForm
  std::function<float(float)> m_efficiencyFcn;
  
  //m_expOfLogPowerSeriesCoeffs: the coefficients for the intrinsic efficiency
  //  when m_efficiencyForm==kExpOfLogPowerSeries
  std::vector<float> m_expOfLogPowerSeriesCoeffs;
 
  /** Valid if same size as m_expOfLogPowerSeriesCoeffs. */
  std::vector<float> m_expOfLogPowerSeriesUncerts;
  
  //In order to keep track of lineage and uniqueness of detectors, we will use
  //  hash values.  All non-serialization related non-const member functions
  //  should recompute the m_hash value when/if it makes any changes to the
  //  detector.
  uint64_t m_hash;
  
  //The m_parentHash is supposed to help track lineage of the detector.  If
  //  retrieve a detector from a database or spectrum file, and then alter and
  //  use it, you should set m_parentHash to the original value of m_hash you
  //  loaded the detector from.  This is not managed in this class; you must
  //  do this.
  //  (we may work out a better system once we actually implement modifying
  //   detectors in InterSpec)
  uint64_t m_parentHash;
  
  /** Not currently used, but in place for future upgrades, so DB schema wont
   have to be changed.
   
   20230917: now used when serializing to DB, to denote if `m_fixedGeometry` is true.
   20231011: updated from boolean `m_fixedGeometry`, to cover `m_geomType`
   */
  uint64_t m_flags;
  
  /** The lower energy (in keV) the DRF is good to; not (currently) enforced
   anywhere in InterSpec, but is good to know.
   */
  double m_lowerEnergy;
  
  /** The upper energy (in keV) the DRF is good to; not (currently) enforced
   anywhere in InterSpec, but is good to know.
   */
  double m_upperEnergy;
  
  /** Time when DRF was created. */
  int64_t m_createdUtc;
  
  /** Last time the DRF was used. */
  int64_t m_lastUsedUtc;
  
  /** Sets the geometry type this DRF describes.
   
   Default is far-field, but may also be one of the fixed-geometry types.
   */
  EffGeometryType m_geomType;
  
  /** On 20230916 updated from version 0 to 1, to account for `m_fixedGeometry` - will still write version 0 if
   `m_geomType == EffGeometryType::FarField`.
   
   On 20240410 updated from 1 to 2, to account for `ResolutionFnctForm::kConstantPlusSqrtEnergy` type of FWHM
   being added.  However, will only write 2 if `m_resolutionForm == ResolutionFnctForm::kConstantPlusSqrtEnergy`.
   */
  static const int sm_xmlSerializationVersion;
  
public:
  
  template<class Action>
  void saveFloatVectorToDB( std::vector<float> &vFloat,
                            const std::string &dbname, Action &a )
  {
    try
    {
      std::stringstream ssv;
      for( size_t i = 0; i < vFloat.size(); ++i )
        ssv << (i?" ":"") << vFloat[i];
      std::string result = ssv.str();
      Wt::Dbo::field( a, result, dbname );
    }catch( std::exception & )
    {
      throw std::runtime_error( "Error saving field " + dbname
                                + " of DetectorPeakResponse to database." );
    } //catch
  }//saveFloatVectorToDB(...)
  
  template<class Action>
  void loadDBToFloatVector( std::vector<float> &vFloat,
                            const std::string &dbname, Action &a )
  {
    vFloat.clear();
    
    std::string result;
    Wt::Dbo::field( a, result, dbname );
    
    SpecUtils::split_to_floats( result.c_str(), result.size(), vFloat );
  } //loadDBToFloatVector(std::vector<float> vFloat, std::string dbname, Action a)
  
  template<class Action>
  void saveEnergyEfficiencyPairVectorToDB(
                                  std::vector<EnergyEfficiencyPair> &effPair,
                                  const std::string &dbname, Action &a )
  {
    try
    {
      std::stringstream ssv;
      for( size_t i = 0; i < effPair.size(); ++i )
        ssv << (i?" ":"") << effPair[i].energy << " " << effPair[i].efficiency;
      std::string result = ssv.str();
      Wt::Dbo::field( a, result, dbname );
    }catch( std::exception & )
    {
      throw std::runtime_error( "Error saving field " + dbname + " to database" );
    } //catch
  }//saveEnergyEfficiencyPairVectorToDB(...)
  
  
  template<class Action>
  void loadDBToEnergyEfficiencyPairVector(
                                    std::vector<EnergyEfficiencyPair> &effPair,
                                    const std::string &dbname, Action &a )
  {
    effPair.clear();
    
    std::string result;
    Wt::Dbo::field( a, result, dbname );
    
    std::vector<float> vals;
    SpecUtils::split_to_floats( result.c_str(), result.size(), vals );
    
    if( (vals.size()%2) != 0 )
      throw std::runtime_error( dbname + " field of DetectorPeakResponse "
                                "database entry had invalid number of fields" );
    
    for( size_t i = 0; i < vals.size(); i += 2 )
	{
	  EnergyEfficiencyPair val;
	  val.energy = vals[i];
	  val.efficiency = vals[i+1];
	  effPair.push_back( val );
      //effPair.push_back( EnergyEfficiencyPair{vals[i],vals[i+1]} );
	}
  }// void loadDBToEnergyEfficiencyPairVector(...)
  
  template<class Action>
  void persist( Action &a )
  {
    Wt::Dbo::field( a, m_name, "m_name", 255 );
    Wt::Dbo::field( a, m_description, "m_description", 255 );
    Wt::Dbo::field( a, m_detectorDiameter, "m_detectorDiameter" );
    Wt::Dbo::field( a, m_efficiencyEnergyUnits, "m_efficiencyEnergyUnits" );
    Wt::Dbo::field( a, m_resolutionForm, "m_resolutionForm" );
    
    if( a.getsValue() )
      saveFloatVectorToDB(m_resolutionCoeffs, "m_resolutionCoeffs", a);
    if( a.setsValue() || a.isSchema() )
      loadDBToFloatVector(m_resolutionCoeffs, "m_resolutionCoeffs", a);
    
    Wt::Dbo::field( a, m_efficiencySource, "m_efficiencySource" );
    Wt::Dbo::field( a, m_efficiencyForm, "m_efficiencyForm" );

    if( a.getsValue() )
      saveEnergyEfficiencyPairVectorToDB(m_energyEfficiencies, "m_energyEfficiencies", a);
    if( a.setsValue() || a.isSchema() )
      loadDBToEnergyEfficiencyPairVector(m_energyEfficiencies, "m_energyEfficiencies", a);
    
    Wt::Dbo::field( a, m_efficiencyFormula, "m_efficiencyFormula" );
    
    if( (a.setsValue() || a.isSchema()) && m_efficiencyFormula.size() )
    {
      const bool isMeV = (m_efficiencyEnergyUnits > 10.0f);
      
      try
      {
        m_efficiencyFcn
            = DetectorPeakResponse::makeEfficiencyFunctionFromFormula( m_efficiencyFormula, isMeV);
      }catch( std::exception & )
      {
        //In principle this shouldnt happen - in practice it might
        m_efficiencyFcn = std::function<float(float)>();
        if( m_efficiencyFormula.find( "invalid formula:" ) == std::string::npos )
          m_efficiencyFormula = "invalid formula: " + m_efficiencyFormula;
      }//try / catch
    }//if( reading from DB )
    
    if( a.getsValue() )
      saveFloatVectorToDB(m_expOfLogPowerSeriesCoeffs, "m_expOfLogPowerSeriesCoeffs", a);
    if( a.setsValue() || a.isSchema() )
      loadDBToFloatVector(m_expOfLogPowerSeriesCoeffs, "m_expOfLogPowerSeriesCoeffs", a);
    
    Wt::Dbo::field( a, m_user, "InterSpecUser_id" );
    
    //Wt::Dbo doesnt support unsigned integers, so we got a little workaround
    //
    //  20230916: We'll store `m_fixedGeometry` as a bit in `m_flags`, to avoid
    //            bothering to change the schema
    //  20231010: Updating fixed geometry from a bool to an enum, `m_geomType`
    //const uint64_t fixed_geom_bit = 0x80000000;
    const uint64_t fixed_geom_bits =  0xF80000000;
    
    int64_t hash = 0, parentHash = 0, flags = 0;
    if( a.getsValue() )
    {
      hash = reinterpret_cast<int64_t&>(m_hash);
      parentHash = reinterpret_cast<int64_t&>(m_parentHash);
      
      //uint64_t flags_tmp = (m_flags | (m_fixedGeometry ? fixed_geom_bit : uint64_t(0)));
      
      // A sanity check to check development consistency
      static_assert( (static_cast<uint64_t>(EffGeometryType::FixedGeomTotalAct) << 31) == 0x80000000, "" );
      
      uint64_t geom_flags = static_cast<uint64_t>( m_geomType );
      geom_flags = (geom_flags << 31);  //31 bits, because the equivalent of EffGeometryType::FixedGeomTotalAct was set to this during development
      assert( geom_flags == (geom_flags & fixed_geom_bits) );
      assert( (m_geomType == EffGeometryType::FarField) || (geom_flags != 0) );
      
      uint64_t flags_tmp = (m_flags | geom_flags);
      flags = reinterpret_cast<int64_t&>(flags_tmp);
    }//if( a.getsValue() )
    
    
    Wt::Dbo::field( a, hash, "Hash" );
    Wt::Dbo::field( a, parentHash, "ParentHash" );
    Wt::Dbo::field( a, flags, "m_flags" );
    
    if( a.setsValue() || a.isSchema() )
    {
      m_hash = reinterpret_cast<uint64_t&>(hash);
      m_parentHash = reinterpret_cast<uint64_t&>(parentHash);
      m_flags = reinterpret_cast<uint64_t&>(flags);
      
      //m_fixedGeometry = (m_flags & fixed_geom_bit);
      //m_flags &= ~fixed_geom_bit; //clear fixed
      
      const uint64_t geom_flags = ((reinterpret_cast<uint64_t&>(flags) & fixed_geom_bits) >> 31);
      const EffGeometryType geom_type = static_cast<EffGeometryType>( geom_flags );
      m_flags &= ~fixed_geom_bits; //clear fixed
      
      if( !a.isSchema() )
      {
        assert( (geom_type == EffGeometryType::FarField)
               || (geom_type == EffGeometryType::FixedGeomTotalAct)
               || (geom_type == EffGeometryType::FixedGeomActPerCm2)
               || (geom_type == EffGeometryType::FixedGeomActPerM2)
               || (geom_type == EffGeometryType::FixedGeomActPerGram) );
        
        if( (geom_type != EffGeometryType::FarField)
           && (geom_type != EffGeometryType::FixedGeomTotalAct)
           && (geom_type != EffGeometryType::FixedGeomActPerCm2)
           && (geom_type != EffGeometryType::FixedGeomActPerM2)
           && (geom_type != EffGeometryType::FixedGeomActPerGram) )
        {
          throw std::runtime_error( "Invalid geometry flags read in DetectorPeakResponse::persist ("
                                   + std::to_string(static_cast<uint64_t>(geom_type)) + ")" );
        }
      }//if( !a.isSchema() )
      
      switch( geom_type )
      {
        case EffGeometryType::FarField:
        case EffGeometryType::FixedGeomTotalAct:
        case EffGeometryType::FixedGeomActPerCm2:
        case EffGeometryType::FixedGeomActPerM2:
        case EffGeometryType::FixedGeomActPerGram:
          m_geomType = geom_type;
          break;
      }//switch( geom_type )
      
    }//if( a.setsValue() || a.isSchema() )

    if( a.getsValue() )
      saveFloatVectorToDB(m_expOfLogPowerSeriesUncerts, "m_expOfLogPowerSeriesUncerts", a);
    if( a.setsValue() || a.isSchema() )
      loadDBToFloatVector(m_expOfLogPowerSeriesUncerts, "m_expOfLogPowerSeriesUncerts", a);
    
    if( a.getsValue() )
      saveFloatVectorToDB(m_resolutionUncerts, "m_resolutionUncerts", a);
    if( a.setsValue() || a.isSchema() )
      loadDBToFloatVector(m_resolutionUncerts, "m_resolutionUncerts", a);
    
    Wt::Dbo::field( a, m_lowerEnergy, "m_lowerEnergy" );
    Wt::Dbo::field( a, m_upperEnergy, "m_upperEnergy" );
    Wt::Dbo::field( a, m_createdUtc, "m_createdUtc" );
    Wt::Dbo::field( a, m_lastUsedUtc, "m_lastUsedUtc" );
  } //void persist( Action &a )
};//class DetectorPeakResponse


#endif  //DetectorPeakResponse_h
