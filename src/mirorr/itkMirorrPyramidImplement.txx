/*=========================================================================
Program: mirorr
Module: itkMirorrPyramidImplement.txx
Author: Nicholas Dowson
Created: 20 Feb 2009

Copyright (c) 2009-15 CSIRO. All rights reserved.

For complete copyright, license and disclaimer of warranty
information see the LICENSE.txt file for details.

This software is distributed WITHOUT ANY WARRANTY; without even
the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
PURPOSE.  See the above copyright notice for more information.
=========================================================================*/
//History
// 24 Apr 2009: Moving small error in the way the parameters are updated

#ifndef __itkMirorrPyramidImplement_txx
#define __itkMirorrPyramidImplement_txx

#include "itkMirorrPyramidImplement.h"
#include <itkCommand.h>
#include <itkResampleImageFilter.h>
#include <itkImageFileWriter.h>
#include <boost/timer/timer.hpp>
#include <string>
#include <iomanip>
#include <exception>
#include "itkIOUtils.h"


//  The following section of code implements an observer
//  that will monitor the evolution of the registration process.
//
class CommandIterationUpdate2 : public itk::Command
{
public:
  typedef  CommandIterationUpdate2   Self;
  typedef  itk::Command             Superclass;
  typedef  itk::SmartPointer<Self>  Pointer;
  itkNewMacro( Self );
protected:
  CommandIterationUpdate2() {};
public:
  typedef   MirorrPyramidImplement::SecondOptimizerType  OptimizerType;
  typedef   const OptimizerType * OptimizerPointer;

  void Execute(itk::Object *caller, const itk::EventObject & event)
  {
    Execute( (const itk::Object *)caller, event);
  }

  void Execute(const itk::Object * object, const itk::EventObject & event)
  {
    OptimizerPointer optimizer =
        dynamic_cast< OptimizerPointer >( object );
    if( !(itk::IterationEvent().CheckEvent( &event )) )
    {
      return;
    }
    std::cout << optimizer->GetCurrentIteration() << "   ";
    std::cout << optimizer->GetValue() << "   ";
    std::cout << optimizer->GetCurrentPosition() << std::endl;
  }
};


MirorrPyramidImplement
::MirorrPyramidImplement()
:
minLength( 32 ) //This ensures that minimum length along
//one side of image is not smaller than 32 voxels
{
  interpolator = InterpolatorType::New();
  mask_interpolator = MaskInterpolatorType::New();

  // DRH: this is for symmetrical registration:
  fixedmask_interpolator = MaskInterpolatorType::New();
  interpolatorFixed = InterpolatorType::New();

  verbosity          = 1;

  this->registrationType = ERegSymmetrical;
  this->InitializeRegistrationObjet(); //Initialise this->registration
  this->resamplingType = EResamplingMiddle;

  halveIterations = false;

  m_PyramidScheduleTuner = PyramidScheduleTunerType::New();

  // By default we do not perform secondary registration
  m_LevelToChangeMethod = 1;
  m_UseBlockMatchingAlgorithm = true;
  m_UseMutualInformationAlgorithm = true;

  // Set up secondary registration (ITK)
  m_SecondOptimizer    = SecondOptimizerType::New();
  m_SecondRegistration = SecondRegistrationType::New();
  m_SecondMetric = 0;
  this->secondRegistrationType = ERegItkMMI; // Mattes Mutual Information metric by default

  //Default is 10%
  m_RequestedSampleRate = 0.1;

#ifdef USE_NPW
  registration->SetNPWbins( 32 ); //inputs.NPWbins );
#ifndef USE_OPENCL
  registration->SetNPWscaleFactor( 2 ); //inputs.NPWscaleFactor );
  registration->SetNPWshapeExpansion( false ); //inputs.NPWshapeExpansion );
#endif
#endif

}

void
MirorrPyramidImplement
::SetSecondaryRegistrationMetricToMMI(){
  this->secondRegistrationType = ERegItkMMI;
}

void
MirorrPyramidImplement
::SetSecondaryRegistrationMetricToNCC(){
  this->secondRegistrationType = ERegItkNCC;
}

void
MirorrPyramidImplement
::SetSecondaryRegistrationMetricToGD(){
  this->secondRegistrationType = ERegItkGD;
}

void
MirorrPyramidImplement
::SetSecondaryRegistrationMetricToMI() {
  this->secondRegistrationType = ERegItkMI;
}

void
MirorrPyramidImplement
::CreateSecondaryRegistrationMetric()
{
  //Secondary ITK registration: metric
  switch (this->secondRegistrationType)
  {
    case ERegItkMMI: {
      SecondMetricTypeMMI::Pointer p = SecondMetricTypeMMI::New();
      p->SetNumberOfHistogramBins( 64 );
      m_SecondMetric = p;
    } break;
    case ERegItkNCC: {
      SecondMetricTypeNCC::Pointer p = SecondMetricTypeNCC::New();
      m_SecondMetric = p;
    } break;
    case ERegItkGD: {
      SecondMetricTypeGD::Pointer p = SecondMetricTypeGD::New();
      m_SecondMetric = p;
    } break;
    case ERegItkMI: {
      SecondMetricTypeMI::Pointer p = SecondMetricTypeMI::New();
      m_SecondMetric = p;
    } break;
    default:
      std::cerr << "Error: Unknown secondary registration type." << std::endl;
      exit(1);
  }
}

void
MirorrPyramidImplement
::RunMirorr( /*typename*/ TransformType::Pointer iTransform )
{
  //Check moving and fixed image have been assigned
  if( movingImage.IsNull() )
    throw std::runtime_error( "RunMirorr Exception: Moving Image has not been assigned." );
  //itkExceptionMacro( "RunMirorr Exception: Moving Image has not been assigned." );
  if( fixedImage.IsNull() )
    throw std::runtime_error( "RunMirorr Exception: Fixed Image has not been assigned." );
  //itkExceptionMacro( "RunMirorr Exception: Fixed Image has not been assigned." );

  //Create the image pyramid
  CreateImagePyramids();

  SetupRegistrationObjetResamplingMode();

  //Initialize with identity transform. How can we set this properly?
  registration->SetTransform( iTransform );
  ParametersType currentParameters( iTransform->GetParameters() );

  const unsigned int n_levels = movingPyramid->GetNumberOfLevels();

  //Check change-over level is >= min and <= max
  unsigned int t_min = m_PyramidScheduleTuner->GetCroppedLevel(m_PyramidScheduleTuner->GetLevelMin());
  unsigned int t_max = m_PyramidScheduleTuner->GetCroppedLevel(m_PyramidScheduleTuner->GetLevelMax(),movingPyramid->GetNumberOfLevels());

  //Check on if and when we should change from the block matching method to the ITK method
  unsigned int t = m_PyramidScheduleTuner->GetCroppedLevel(m_LevelToChangeMethod, t_max);

  if( t<t_min )
  {
    std::cout<<"WARNING: Change level ("<<m_LevelToChangeMethod<<"->"<<t<<") < min level ("
        <<t_min<<"). Setting to min."<<std::endl;
    t = t_min;
  }
  if( t>t_max )
  {
    std::cout<<"WARNING: Change level ("<<m_LevelToChangeMethod<<"->"<<t<<") > max level ("
        <<t_max<<"). Setting to max."<<std::endl;
    t = t_max;
  }
  m_LevelToChangeMethod = t;
  unsigned int max_block_matching_level = m_LevelToChangeMethod - t_min + 1;
  if( verbosity >= 1 )
    std::cout << "Switching after level: " << max_block_matching_level
	      << " of " << n_levels
	      << "  BlockMatching?: " << m_UseBlockMatchingAlgorithm
	      << "  MutualInfo?: " << m_UseMutualInformationAlgorithm
    << std::endl;

  boost::timer::cpu_timer level_timer, total_timer;

  //Now run the registration algorithm
  int first_max_iterations = registration->GetMaxIterations();
  double portionMatchesKept = registration->GetPortionMatchesKept();
  if( m_UseBlockMatchingAlgorithm )
  {
    if( verbosity >= 1 )
      std::cout << "\nPrimary alignment using Block Matching registration =================\n" << std::endl;
    int max_iterations = registration->GetMaxIterations();

    //EXCLUSIVE pyramid level
    for( unsigned int level=0; level< max_block_matching_level; ++level )
    {
      //Get the current images
      registration->SetMovingImage( movingPyramid->GetOutput(level) );
      registration->SetFixedImage( fixedPyramid->GetOutput(level) );
      registration->SetMovingMask(movingMask);
      registration->SetFixedMask(fixedMask);

      registration->SetMovingImageRegion( movingImage->GetLargestPossibleRegion() );
      if( level <= 1 ) //For small images, use all available data
        registration->SetPortionMatchesKept( 1.0 );
      else
        registration->SetPortionMatchesKept( portionMatchesKept );

      //Set the parameters
      registration->GetTransform()->SetParameters( currentParameters );

      //Set the number of iterations
      if( max_iterations != 0 )
        registration->SetMaxIterations( max_iterations );
      if( halveIterations )
        max_iterations = std::max( 1, max_iterations/2 );

      //Tell user what is going on
      if( verbosity >= 1 )
        DisplayCurrentLevel( level + t_min -1);

      //Run registration
      registration->Update();

      //Update the current parameters
      //ParametersType lastParameters(currentParameters);
      currentParameters = registration->GetLastTransformParameters();

      if( verbosity >= 1 ) {
        DisplayLevelTimings( level, level_timer, total_timer );
        std::cout << std::endl;
      }

    }
  } else {
    if( verbosity >= 1 ) {
      std::cout << "Skipping block matching step: not requested by the user." << std::endl;
      std::cout << "  '-> Note: levels " << 1 << " to " << max_block_matching_level << " NOT processed with the block matching algorithm." << std::endl;
    }
  }

  if( verbosity >= 1)
    std::cout << "Block Match Registration DONE" << std::endl;

  iTransform->SetParameters( currentParameters );

  //Reset iterations
  registration->SetMaxIterations( first_max_iterations );

  //SECONDARY REGISTRATION =======================================
  //Set up secondary registration
  if( m_UseMutualInformationAlgorithm )
  {
    this->CreateSecondaryRegistrationMetric();
    m_SecondRegistration->SetOptimizer(    m_SecondOptimizer );
    m_SecondRegistration->SetInterpolator( interpolator      );
    m_SecondRegistration->SetMetric(       m_SecondMetric    );

    if( verbosity >= 1 )
    {
      std::cout << "\nRefining using ITK registration =================" << std::endl;
      CommandIterationUpdate2::Pointer observer = CommandIterationUpdate2::New();
      m_SecondOptimizer->AddObserver( itk::IterationEvent(), observer );
    }

#if ITK_VERSION_MAJOR < 4
    if(movingMask)
      std::cerr<<"WARNING pre-ITK4 compilations do NOT support masks as spatial "
          "objects! Please do not specify moving mask."<<std::endl;
    if(fixedMask)
      std::cerr<<"WARNING pre-ITK4 compilations do NOT support masks as spatial "
          "objects! Please do not specify fixed mask."<<std::endl;
#else
    MaskObjectType::Pointer fm = MaskObjectType::New();
    fm->SetImage( movingMask );
    m_SecondMetric->SetFixedImageMask(fm);

    MaskObjectType::Pointer mm = MaskObjectType::New();
    mm->SetImage( fixedMask );
    m_SecondMetric->SetMovingImageMask(mm);
#endif

    SecondOptimizerType::ScalesType scales( currentParameters.GetNumberOfElements() );

    for( unsigned int level= max_block_matching_level -1; level<n_levels; ++level )
    {
      //Get the current images
      m_SecondRegistration->SetMovingImage( fixedPyramid->GetOutput(level) );
      m_SecondRegistration->SetFixedImage(  movingPyramid->GetOutput(level) );
      m_SecondRegistration->SetFixedImageRegion( movingPyramid->GetOutput(level)->GetLargestPossibleRegion() );
      m_SecondRegistration->SetTransform(    iTransform        );
      m_SecondRegistration->GetTransform()->SetParameters( currentParameters );
      m_SecondRegistration->SetInitialTransformParameters( currentParameters );

      //Sampling
      //m_SecondMetric->SetNumberOfSpatialSamples( 150000 );
      //m_SecondMetric->ReinitializeSeed( 76926294 );
      if(m_RequestedSampleRate>=1.0 || m_RequestedSampleRate<=0.0)
        m_SecondMetric->UseAllPixelsOn();
      else
      {
        double n_samples3 = movingPyramid->GetOutput(level)->GetLargestPossibleRegion().GetNumberOfPixels();
        double n_samples =  n_samples3 * m_RequestedSampleRate;
        //double n_samples2 = std::pow(double(m_SecondMetric->GetNumberOfHistogramBins()),0.67)*1000.0;
        double n_samples2 = std::pow(64.,0.67)*1000.0; //DRH 2013-12-05: this is a lower bound based on a 64bins MI Histogram. No clue about the 0.67 and 1000.0 numbers.
        n_samples = std::max( n_samples, n_samples2 );
        if( n_samples < n_samples3 )
          m_SecondMetric->SetNumberOfSpatialSamples( n_samples );
        else
          m_SecondMetric->UseAllPixelsOn();
      }

      //Set up the optimizer and scaling
      m_SecondOptimizer->SetMaximize( false );
      //m_SecondOptimizer->SetStepLength( 100.0 );
      //m_SecondOptimizer->SetMaximumStepLength( 0.1 ); //MILXREG 2
      //m_SecondOptimizer->SetMinimumStepLength( 0.01 ); //MILXREG 0.2

      for( unsigned int ii=0, jj=0; ii<scales.GetNumberOfElements(); ++ii )
        if( ii<scales.GetNumberOfElements()-3 ) //NON-TRANSLATION:
        {
          const double max_movement = 5e-2;
          scales[ii] = 1.0 / max_movement;      //  We may move up 0.05 radians
        }
        else                                    //TRANSLATION:
        {                                       // we may move up to 5 voxels
          const double max_movement = 5.0;

          scales[ii] = 1.0 /
              (max_movement * m_SecondRegistration->GetFixedImage()->GetSpacing()[jj]);
          ++jj;
        }
      m_SecondOptimizer->SetScales( scales );

      //Tell user what is going on
      if( verbosity >= 1 ) DisplayCurrentLevel( level + t_min - 1);

      //Run registration
#if ITK_VERSION_MAJOR < 4
      m_SecondRegistration->StartRegistration();
#else
      m_SecondRegistration->Update();
#endif
      

      //Update the current parameters
      currentParameters = m_SecondRegistration->GetLastTransformParameters();

      if( verbosity >= 1 ) DisplayLevelTimings( level, level_timer, total_timer );
    }
    iTransform->SetParameters( m_SecondRegistration->GetLastTransformParameters() );
  } else {
    if( verbosity >= 1 && (max_block_matching_level != n_levels)) {
      std::cout << "Skipping ITK 'Mutual Information' refinement steps: not requested by the user." << std::endl;
      std::cout << "  '-> Note: levels " << max_block_matching_level << " to " << n_levels << " NOT processed with ITK." << std::endl;
    }
  }

  //Extract the optimized parameters
  if(verbosity >= 1)
    std::cout << "Best Pos: " << std::setprecision(5) << itk::PrettyPrint(iTransform->GetParameters()) << std::endl;
}

void
MirorrPyramidImplement
::SetRegistrationType(ERegistrationType rtype)
{
  if (this->registrationType != rtype) {
    this->registrationType = rtype;
    this->InitializeRegistrationObjet();
  }
}

void
MirorrPyramidImplement
::SetRegistrationTypeClassic()
{
  if (this->registrationType != ERegClassic) {
      this->registrationType = ERegClassic;
      this->InitializeRegistrationObjet();
    }
}

void
MirorrPyramidImplement
::SetRegistrationTypeSymmetrical()
{
  if (this->registrationType != ERegSymmetrical) {
    this->registrationType = ERegSymmetrical;
    this->InitializeRegistrationObjet();
  }
}

MirorrPyramidImplement::ERegistrationType
MirorrPyramidImplement
::GetRegistrationType()
{
  return this->registrationType;
}

void
MirorrPyramidImplement
::SetResamplingType(EResamplingType rtype)
{
  this->resamplingType = rtype;
}

MirorrPyramidImplement::EResamplingType
MirorrPyramidImplement
::GetResamplingType()
{
  return this->resamplingType;
}


//template <unsigned int DIMENSION>
void
MirorrPyramidImplement/*<DIMENSION>*/
::DisplayCurrentLevel( int level )
{
  std::cout << "Pyramid level: " << level+1 << std::endl;

  if( verbosity >= 2 ) {
    //Moving image dimensions
    displayImgStats("Moving ", movingPyramid->GetOutput(level) );
    displayImgStats("Fixed ", fixedPyramid->GetOutput(level) );

    //Subsampling
    std::cout << "Moving Shrink Factor: ";
    for( unsigned int ii=0; ii<DIMENSION; ++ii )
      std::cout << movingPyramid->GetSchedule()[level][ii] << " ";
    std::cout << "  Fixed Shrink Factor: ";
    for( unsigned int ii=0; ii<fixedImage->GetImageDimension(); ++ii )
      std::cout << fixedPyramid->GetSchedule()[level][ii] << " ";
    std::cout << std::endl;
  }
}

//template <unsigned int DIMENSION>
void
MirorrPyramidImplement/*<DIMENSION>*/
::DisplayLevelTimings( int level, boost::timer::cpu_timer &level_timer, boost::timer::cpu_timer &total_timer )
{
  std::cout << "Level " << level+1 << " completed in "
  << std::setprecision(2) << level_timer.elapsed().wall/1.E9 << "s of "
  << std::setprecision(2) << total_timer.elapsed().wall/1.E9
  << "s <-^------"
  << std::endl;
  level_timer.start();
}

//template <unsigned int DIMENSION>
/*typename*/ MirorrPyramidImplement/*<DIMENSION>*/::ImagePointer
MirorrPyramidImplement/*<DIMENSION>*/
::GetResampledImage(
    /*typename*/ TransformType::Pointer transform,
    bool resample_moving
)
{
  //Create an image resampler
  typedef itk::ResampleImageFilter< ImageType, ImageType > ResampleFilterType;
  /*typename*/ ResampleFilterType::Pointer resampler = ResampleFilterType::New();

  resampler->SetDefaultPixelValue( 0 );
  resampler->SetUseReferenceImage(true);

  if( resample_moving )
  {
    resampler->SetInput( movingImage );
    resampler->SetReferenceImage(fixedImage);
    resampler->SetTransform( dynamic_cast<TransformType*>( transform->GetInverseTransform().GetPointer() ) );
  }
  else
  {
    resampler->SetInput( fixedImage );
    resampler->SetReferenceImage(movingImage);
    resampler->SetTransform( transform );
  }

  resampler->Update();
  ImagePointer resampledFixedImage = resampler->GetOutput();

  return resampledFixedImage;
}


void
MirorrPyramidImplement
::InitializeRegistrationObjet()
{
  switch (this->registrationType)
  {
    case ERegClassic : {
      this->registration = RegistrationType::New();
      this->registration->SetInterpolator( interpolator );
      this->registration->SetMaskInterpolator( mask_interpolator );
    }
    break;
    case ERegSymmetrical : {
      SymRegistrationType::Pointer reg = SymRegistrationType::New();
      reg->SetInterpolator( interpolator );
      reg->SetMaskInterpolator( mask_interpolator );
      reg->SetFixedImageInterpolator(interpolatorFixed);
      reg->SetFixedMaskInterpolator(fixedmask_interpolator);
      this->registration = reg;
    }
    break;
    default: {
      std::stringstream ss;
      ss
       << "ERROR: MirorrPyramidImplement::InitializeRegistrationObjet() "
       << " Unknown registration type. This should not happen..." << std::endl;
      throw std::runtime_error(ss.str());
    }
  }

  registration->SetVerbosity(verbosity);
}

void
MirorrPyramidImplement
::SetupRegistrationObjetResamplingMode()
{
  // Assuming: not(movingImage.IsNull()) and not(fixedImage.IsNull())
  //=0, EResamplingMiddle, EResamplingMaxResolution, EResamplingMaxSize

  switch(this->resamplingType) {
  case EResamplingBasic:
    this->registration->SetResamplingType(RegistrationType::EResamplingBasic);
    break;
  case EResamplingMiddle:
    this->registration->SetResamplingType(RegistrationType::EResamplingMiddle);
    break;
  case EResamplingMaxResolution: {
      ImageType::SpacingType sF = this->fixedImage->GetSpacing();
      ImageType::SpacingType sM = this->movingImage->GetSpacing();
      double dF = sF.GetSquaredNorm();
      double dM = sM.GetSquaredNorm();

      if (dF < dM) {
        this->registration->SetResamplingType(RegistrationType::EResamplingFixed);
      } else {
        this->registration->SetResamplingType(RegistrationType::EResamplingMoving);
      }
    }
    break;
  case EResamplingMaxSize: {
      ImageType::RegionType::SizeType sF = this->fixedImage->GetLargestPossibleRegion().GetSize();
      ImageType::RegionType::SizeType sM = this->movingImage->GetLargestPossibleRegion().GetSize();
      double vF = 1., vM = 1.;
      for (unsigned int i=0; i<ImageType::RegionType::SizeType::Dimension; i++) {
        vF *= sF[i];
        vM *= sM[i];
      }

      if (vF > vM) {
        this->registration->SetResamplingType(RegistrationType::EResamplingFixed);
      } else {
        this->registration->SetResamplingType(RegistrationType::EResamplingMoving);
      }
    }
    break;
  case EResamplingFixed:
    this->registration->SetResamplingType(RegistrationType::EResamplingFixed);
    break;
  case EResamplingMoving:
    this->registration->SetResamplingType(RegistrationType::EResamplingMoving);
    break;
  default:  {
    std::stringstream ss;
    ss
     << "ERROR: MirorrPyramidImplement::SetupRegistrationObjetResamplingMode() "
     << " Unknown resampling type. This should not happen..." << std::endl;
    throw std::runtime_error(ss.str());
    }
    break;
  }
}

//template <unsigned int DIMENSION>
void
MirorrPyramidImplement
::CreateImagePyramids()
{
  //Generate a decent pyramid schedule
  m_PyramidScheduleTuner->SetFixedSpacing( fixedImage->GetSpacing() );
  m_PyramidScheduleTuner->SetMovingSpacing( movingImage->GetSpacing() );
  m_PyramidScheduleTuner->SetFixedSize( fixedImage->GetLargestPossibleRegion().GetSize() );
  m_PyramidScheduleTuner->SetMovingSize( movingImage->GetLargestPossibleRegion().GetSize() );

  m_PyramidScheduleTuner->Update();

  if( verbosity >= 1 ) {
    std::cout << "\nPyramid Schedule: \n";
    m_PyramidScheduleTuner->PrintPyramidInfo(std::cout, verbosity);
    std::cout << "Building Image pyramid..." << std::endl;
  }

  movingPyramid = ImagePyramidFilterType::New();
  fixedPyramid = ImagePyramidFilterType::New();

  movingPyramid->SetInput( movingImage );
  fixedPyramid->SetInput( fixedImage );

  movingPyramid->SetNumberOfLevels( m_PyramidScheduleTuner->GetMovingSchedule().rows() );
  fixedPyramid->SetNumberOfLevels( m_PyramidScheduleTuner->GetFixedSchedule().rows() );

  movingPyramid->SetSchedule( m_PyramidScheduleTuner->GetMovingSchedule() );
  fixedPyramid->SetSchedule( m_PyramidScheduleTuner->GetFixedSchedule() );

  //Create the image pyramids
  movingPyramid->UpdateLargestPossibleRegion();
  fixedPyramid->UpdateLargestPossibleRegion();
}



#endif
