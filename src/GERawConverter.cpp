/** @file GERawConverter.cpp */
#include <iostream>
#include <stdexcept>

#include <libxml/xmlschemas.h>
#include <libxslt/xslt.h>
#include <libxslt/transform.h>
#include <libxslt/xsltutils.h>

// Local
#include "GERawConverter.h"
#include "XMLWriter.h"
#include "ge_tools_path.h"


namespace GEToIsmrmrd {

const std::string g_schema = "\
<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?>                \
<xs:schema xmlns=\"https://github.com/nih-fmrif/GEISMRMRD\"                 \
    xmlns:xs=\"http://www.w3.org/2001/XMLSchema\"                           \
    elementFormDefault=\"qualified\"                                        \
    targetNamespace=\"https://github.com/nih-fmrif/GEISMRMRD\">             \
    <xs:element name=\"conversionConfiguration\">                           \
        <xs:complexType>                                                    \
            <xs:sequence>                                                   \
                <xs:element maxOccurs=\"unbounded\" minOccurs=\"1\"         \
                    name=\"sequenceMapping\" type=\"sequenceMappingType\"/> \
            </xs:sequence>                                                  \
        </xs:complexType>                                                   \
    </xs:element>                                                           \
    <xs:complexType name=\"sequenceMappingType\">                           \
        <xs:all>                                                            \
            <xs:element name=\"psdname\" type=\"xs:string\"/>               \
            <xs:element name=\"libraryPath\" type=\"xs:string\"/>           \
            <xs:element name=\"className\" type=\"xs:string\"/>             \
            <xs:element name=\"stylesheet\" type=\"xs:string\"/>            \
            <xs:element name=\"reconConfigName\" type=\"xs:string\"/>       \
        </xs:all>                                                           \
    </xs:complexType>                                                       \
</xs:schema>";

/**
 * Creates a GERawConverter from an ifstream of the raw data file header
 *
 * @param fp raw FILE pointer to raw data file
 * @throws std::runtime_error if raw data file cannot be read
 */
GERawConverter::GERawConverter(const std::string& rawFilePath, const std::string& classname, bool logging)
    : log_(logging)
{
   psdname_ = ""; // TODO: find PSD Name in Orchestra Pfile class
   log_ << "PSDName: " << psdname_ << std::endl;

   // Use Orchestra to figure out if P-File or ScanArchive
   if (GERecon::ScanArchive::IsArchiveFilePath(rawFilePath))
   {
      scanArchive_ = GERecon::ScanArchive::Create(rawFilePath, GESystem::Archive::LoadMode);
      lxData_ = boost::dynamic_pointer_cast<GERecon::Legacy::LxDownloadData>(scanArchive_->LoadDownloadData());

      boost::shared_ptr<GERecon::Legacy::LxControlSource> const controlSource = boost::make_shared<GERecon::Legacy::LxControlSource>(lxData_);
      processingControl_ = controlSource->CreateOrchestraProcessingControl();

      rawObjectType_ = SCAN_ARCHIVE_RAW_TYPE;
   }
   else
   {
      pfile_ = GERecon::Legacy::Pfile::Create(rawFilePath,
                                              GERecon::Legacy::Pfile::AllAvailableAcquisitions,
                                              GERecon::AnonymizationPolicy(GERecon::AnonymizationPolicy::None));

      lxData_ = pfile_->DownloadData();
      processingControl_ = pfile_->CreateOrchestraProcessingControl();

      rawObjectType_ = PFILE_RAW_TYPE;
   }

   if (!classname.compare("GenericConverter"))
   {
      converter_ = std::shared_ptr<SequenceConverter>(new GenericConverter());
   }
   else if (!classname.compare("NIH2dfastConverter"))
   {
      converter_ = std::shared_ptr<SequenceConverter>(new NIH2dfastConverter());
   }
   else if (!classname.compare("NIHepiConverter"))
   {
      converter_ = std::shared_ptr<SequenceConverter>(new NIHepiConverter());
   }
   else
   {
      std::cerr << "Plugin class name: " << classname << " not implemented. Exiting..." << std::endl;

      exit(EXIT_FAILURE);
   }

   // Testing dumping of raw file header as XML.
   // processingControl_->SaveAsXml("rawHeader.xml");  // As of Orchestra 1.8-1, this is causing a crash, with
                                                    // an incomplete file written.
}

void GERawConverter::useStylesheetFilename(const std::string& filename)
{
    log_ << "Loading stylesheet: " << filename << std::endl;
    std::ifstream stream(filename.c_str(), std::ios::binary);
    useStylesheetStream(stream);
}

void GERawConverter::useStylesheetStream(std::ifstream& stream)
{
    stream.seekg(0, std::ios::beg);

    std::string sheet((std::istreambuf_iterator<char>(stream)),
            std::istreambuf_iterator<char>());
    useStylesheetString(sheet);
}

void GERawConverter::useStylesheetString(const std::string& sheet)
{
    stylesheet_ = sheet;
}

/**
 * Converts the XSD ISMRMRD XML header object into a C++ string
 *
 * @returns string represenatation of ISMRMRD XML header
 * @throws std::runtime_error
 */
std::string GERawConverter::getIsmrmrdXMLHeader() {
    try {
        if (stylesheet_.empty()) {
            throw std::runtime_error("No stylesheet configured.");
        }

        std::string ge_raw_file_header;
        try {
            ge_raw_file_header = ge_header_to_xml(lxData_, processingControl_);
            if (ge_raw_file_header.empty()) {
                throw std::runtime_error("Generated GE header is empty.");
            }
        } catch (const std::exception& e) {
            throw std::runtime_error(std::string("Failed to generate GE header from lxData and processingControl: ") + e.what());
        }

        // DEBUG: Output the converted header as XML string
        std::cout << "Converted header as XML string is: " << ge_raw_file_header << std::endl;

        try {
            xmlSubstituteEntitiesDefault(1);
            xmlLoadExtDtdDefaultValue = 1;
        } catch (const std::exception& e) {
            throw std::runtime_error(std::string("Failed to set XML parser defaults: ") + e.what());
        }

        xmlDocPtr stylesheet_doc = nullptr;
        try {
            stylesheet_doc = xmlParseMemory(stylesheet_.c_str(), stylesheet_.size());
            if (stylesheet_doc == nullptr) {
                throw std::runtime_error("Parsed stylesheet_doc is NULL.");
            }
        } catch (const std::exception& e) {
            throw std::runtime_error(std::string("Failed to parse stylesheet from memory: ") + e.what());
        }

        std::shared_ptr<xsltStylesheet> sheet = nullptr;
        try {
            sheet = std::shared_ptr<xsltStylesheet>(xsltParseStylesheetDoc(stylesheet_doc), xsltFreeStylesheet);
            if (!sheet) {
                throw std::runtime_error("Failed to parse XSLT stylesheet.");
            }
        } catch (const std::exception& e) {
            xmlFreeDoc(stylesheet_doc);  // Free if parsing failed
            throw std::runtime_error(std::string("Failed to create shared_ptr for stylesheet: ") + e.what());
        }

        std::shared_ptr<xmlDoc> pfile_doc = nullptr;
        try {
            pfile_doc = std::shared_ptr<xmlDoc>(xmlParseMemory(ge_raw_file_header.c_str(), ge_raw_file_header.size()), xmlFreeDoc);
            if (!pfile_doc) {
                throw std::runtime_error("Failed to parse GE header XML.");
            }
        } catch (const std::exception& e) {
            throw std::runtime_error(std::string("Failed to parse P-File XML: ") + e.what());
        }

        log_ << "Applying stylesheet..." << std::endl;
        std::shared_ptr<xmlDoc> result = nullptr;
        try {
            const char *params[1] = { NULL };
            result = std::shared_ptr<xmlDoc>(xsltApplyStylesheet(sheet.get(), pfile_doc.get(), params), xmlFreeDoc);
            if (!result) {
                throw std::runtime_error("Failed to apply XSLT stylesheet to the document.");
            }
        } catch (const std::exception& e) {
            throw std::runtime_error(std::string("Error applying stylesheet: ") + e.what());
        }

        xmlChar* output = nullptr;
        int len = 0;
        try {
            if (xsltSaveResultToString(&output, &len, result.get(), sheet.get()) < 0) {
                throw std::runtime_error("Failed to save transformed XML to string.");
            }
        } catch (const std::exception& e) {
            throw std::runtime_error(std::string("Error saving result to string: ") + e.what());
        }

        std::string ismrmrd_header((char*)output, len);
        xmlFree(output);
        return ismrmrd_header;
        
    } catch (const std::exception& e) {
        std::cerr << "Error in getIsmrmrdXMLHeader(): " << e.what() << std::endl;
        throw;  // Rethrow the exception to handle it further up if needed
    }
}



/**
 * Gets the acquisitions corresponding to a view in memory.
 *
 * @param view_num View number to get
 * @param vacq Vector of acquisitions
 * @throws std::runtime_error { if plugin fails to copy the data }
 */
std::vector<ISMRMRD::Acquisition> GERawConverter::getAcquisitions(unsigned int view_num)
{
   if (rawObjectType_ == SCAN_ARCHIVE_RAW_TYPE)
   {
      return converter_->getAcquisitions(scanArchive_, view_num);
   }
   else
   {
      return converter_->getAcquisitions(pfile_, view_num);
   }
}

/**
 * Gets the extra field "reconConfig" from the
 * ge-ismrmrd XML configuration. This can be used to
 * add this library to a Gadgetron client
 */
std::string GERawConverter::getReconConfigName(void)
{
    return std::string(recon_config_);
}

std::string GERawConverter::ge_header_to_xml(GERecon::Legacy::LxDownloadDataPointer lxData,
                                             GERecon::Control::ProcessingControlPointer processingControl)
{
    try {
        std::cout << "[INFO] Starting conversion of raw file header to XML string." << std::endl;
        XMLWriter writer;

        writer.startDocument();
        writer.startElement("Header");

        // Step 1: Adding acquisition flags from processing control and lxData
        std::cout << "[INFO] Adding acquisition flags from processing control and lxData." << std::endl;
        writer.addBooleanElement("is3DAcquisition", processingControl->Value<bool>("Is3DAcquisition"));
        writer.addBooleanElement("isCalibration", lxData->IsCalibration());
        writer.addBooleanElement("isAssetCalibration", processingControl->Value<bool>("AssetCalibration"));

        // Step 2: Adding slice and channel info
        std::cout << "[INFO] Adding slice and channel count." << std::endl;
        writer.formatElement("SliceCount", "%d", processingControl->Value<int>("NumSlices"));
        writer.formatElement("ChannelCount", "%d", processingControl->Value<int>("NumChannels"));

        // Step 3: Series information
        std::cout << "[INFO] Fetching DICOM series information." << std::endl;
        GERecon::Legacy::DicomSeries legacySeries(lxData);
        GEDicom::SeriesPointer series = legacySeries.Series();
        GEDicom::SeriesModulePointer seriesModule = series->GeneralModule();
        writer.startElement("Series");

        std::cout << "[INFO] Adding series number and UID." << std::endl;
        writer.formatElement("Number", "%d", processingControl->Value<int>("SeriesNumber"));
        writer.formatElement("UID", "%s", seriesModule->UID().c_str());

        // Series description
        std::cout << "[INFO] Adding series description." << std::endl;
        if (seriesModule->SeriesDescription().empty()) {
            std::cerr << "[WARNING] Series description is empty!" << std::endl;
        }
        writer.formatElement("Description", "%s", seriesModule->SeriesDescription().c_str());

        // Step 4: Study information
        std::cout << "[INFO] Fetching DICOM study information." << std::endl;
        GEDicom::StudyPointer study = series->Study();
        GEDicom::StudyModulePointer studyModule = study->GeneralModule();
        writer.startElement("Study");
        writer.formatElement("Number", "%u", processingControl->Value<int>("ExamNumber"));
        writer.formatElement("UID", "%s", studyModule->UID().c_str());

        // Step 5: Patient information
        std::cout << "[INFO] Adding patient details." << std::endl;
        GEDicom::PatientPointer patient = study->Patient();
        GEDicom::PatientModulePointer patientModule = patient->GeneralModule();
        writer.startElement("Patient");
        writer.formatElement("Name", "%s", patientModule->Name().c_str());
        writer.formatElement("ID", "%s", patientModule->ID().c_str());

        // Step 6: Equipment information
        std::cout << "[INFO] Fetching equipment information." << std::endl;
        GEDicom::EquipmentPointer equipment = series->Equipment();
        GEDicom::EquipmentModulePointer equipmentModule = equipment->GeneralModule();
        writer.startElement("Equipment");
        writer.formatElement("Manufacturer", "%s", equipmentModule->Manufacturer().c_str());

        // Step 7: CoilConfigUID
        std::cout << "[INFO] Fetching CoilConfigUID." << std::endl;
        writer.formatElement("CoilConfigUID", "%u", processingControl->Value<unsigned int>("CoilConfigUID"));

        // Step 8: Image module information
        std::cout << "[INFO] Fetching image module details." << std::endl;
        const GERecon::SliceInfoTable sliceTable = processingControl->ValueStrict<GERecon::SliceInfoTable>("SliceTable");
        auto imageCorners = GERecon::ImageCorners(sliceTable.AcquiredSliceCorners(0), sliceTable.SliceOrientation(0));
        auto dicomImage = GERecon::Legacy::DicomImage(GEDicom::GrayscaleImage(128, 128), 0, imageCorners, series, *lxData);
        auto imageModule = dicomImage.ImageModule();

        writer.startElement("Image");
        writer.formatElement("EchoTime", "%s", imageModule->EchoTime().c_str());
        writer.formatElement("RepetitionTime", "%s", imageModule->RepetitionTime().c_str());

        // Handling EPI-specific data
        if (lxData->IsEpi())
        {
            std::cout << "[INFO] EPI data detected, adding EPI-specific parameters." << std::endl;

            boost::shared_ptr<GERecon::Epi::LxControlSource> controlSource = boost::make_shared<GERecon::Epi::LxControlSource>(lxData);
            GERecon::Control::ProcessingControlPointer procCtrlEPI = controlSource->CreateOrchestraProcessingControl();
            GERecon::Acquisition::ArchiveStoragePointer archive_storage_ptr = GERecon::Acquisition::ArchiveStorage::Create(scanArchive_);

            int ref_views = procCtrlEPI->Value<int>("ExtraFramesTop") + procCtrlEPI->Value<int>("ExtraFramesBottom");

            // In EPI ScanArchive files, the number of acquisitions equals (number of slices per volume + control packet) * number of volumes
            int num_volumes = archive_storage_ptr->AvailableControlCount() / (processingControl->Value<int>("NumSlices") + 1);

            writer.startElement("epiParameters");
            writer.addBooleanElement("isEpiRefScanIntegrated", procCtrlEPI->Value<bool>("IntegratedReferenceScan"));
            writer.addBooleanElement("MultibandEnabled", procCtrlEPI->ValueStrict<bool>("MultibandEnabled"));
            writer.formatElement("ExtraFramesTop", "%d", procCtrlEPI->Value<int>("ExtraFramesTop"));
            writer.formatElement("AcquiredYRes", "%d", procCtrlEPI->Value<int>("AcquiredYRes"));
            writer.formatElement("ExtraFramesBottom", "%d", procCtrlEPI->Value<int>("ExtraFramesBottom"));
            writer.formatElement("NumRefViews", "%d", ref_views);
            writer.formatElement("num_volumes", "%d", num_volumes);
            writer.endElement(); // End of EPI parameters
        }

        writer.endElement(); // End of Image element
        writer.endDocument(); // End of the document

        std::cout << "[INFO] Successfully completed XML generation." << std::endl;
        return writer.getXML();
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Exception caught during XML conversion: " << e.what() << std::endl;
        throw;
    } catch (...) {
        std::cerr << "[ERROR] Unknown error occurred during XML conversion." << std::endl;
        throw;
    }
}

} // namespace GEToIsmrmrd

