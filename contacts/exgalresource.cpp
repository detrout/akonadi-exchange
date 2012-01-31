/*
 * This file is part of the Akonadi Exchange Resource.
 * Copyright 2011 Robert Gruber <rgruber@users.sourceforge.net>, Shaheed Haque
 * <srhaque@theiet.org>.
 *
 * Akonadi Exchange Resource is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Akonadi Exchange Resource is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Akonadi Exchange Resource.
 * If not, see <http://www.gnu.org/licenses/>.
 */

#include "settings.h"
#include "settingsadaptor.h"

#include "exgalresource.h"

#include <akonadi/attributefactory.h>
#include <akonadi/cachepolicy.h>
#include <akonadi/item.h>
#include <akonadi/itemcreatejob.h>
#include <akonadi/itemfetchjob.h>
#include <akonadi/itemmodifyjob.h>
#include <KLocalizedString>
#include <KABC/Address>
#include <KABC/Addressee>
#include <KABC/PhoneNumber>
#include <KABC/Picture>
#include <KDateTime>
#include <KWindowSystem>
#include <QtDBus/QDBusConnection>

#include "mapiconnector2.h"
#include "profiledialog.h"

/**
 * Display ids in the same format we use when stored in Akonadi.
 */
#define ID_BASE 36

/**
 * Set this to 1 to pull all the properties, e.g. to see what a server has
 * available.
 */
#ifndef DEBUG_CONTACT_PROPERTIES
#define DEBUG_CONTACT_PROPERTIES 0
#endif

#define MINUTES_IN_ONE_DAY (60 * 24)

#define FETCH_STATUS "FetchStatus"

using namespace Akonadi;

/**
 * A personal address book entry.
 */
class MapiContact : public MapiMessage, public KABC::Addressee
{
public:
	MapiContact(MapiConnector2 *connection, const char *tallocName, mapi_id_t folderId, mapi_id_t id);

	/**
	 * Fetch all contact properties.
	 */
	virtual bool propertiesPull();

private:
	virtual QDebug debug() const;
	virtual QDebug error() const;

	bool propertiesPull(QVector<int> &tags, const bool tagsAppended, bool pullAll);
};

/**
 * We determine the fetch status of the the GAL by tracking the the age of the
 * collection, and the last fetched item:
 * 
 * 	- By default, GAL clients fetch it once per day. When we finish fetching
 * 	it, we set the current date-time. If fetching is incomplete, the 
 * 	date-time will be invalid.
 * 
 * 	- As fetching proceeds, we store the last fetched item's displayName.
 * 	On resume, this can be used to seek to the right place in the GAL to
 * 	resume fetching.
 */
class FetchStatusAttribute :
	public Akonadi::Attribute
{
public:
	FetchStatusAttribute()
	{
	}

	FetchStatusAttribute(const KDateTime &dateTime, const QString &displayName) :
		m_dateTime(dateTime),
		m_displayName(displayName)
	{
	}

	void setDateTime(const KDateTime dateTime)
	{
		m_dateTime = dateTime;
	}

	KDateTime dateTime() const
	{
		return m_dateTime;
	}

	void setDisplayName(const QString displayName)
	{
		m_displayName = displayName;
	}

	QString displayName() const
	{
		return m_displayName;
	}

	virtual QByteArray type() const
	{
		return FETCH_STATUS;
	}

	virtual Attribute *clone() const
	{
		return new FetchStatusAttribute(m_dateTime, m_displayName);
	}

	virtual QByteArray serialized() const
	{
		static QString separator = QString::fromAscii("|");
		QString tmp = m_dateTime.toString().append(separator).append(m_displayName);

		return tmp.toUtf8();
	}

	virtual void deserialize(const QByteArray &data)
	{
		int i = data.indexOf("|");

		m_dateTime = KDateTime::fromString(QString::fromUtf8(data.left(i)));
		m_displayName = QString::fromUtf8(data.mid(i + 1));
	}

private:
	KDateTime m_dateTime;
	QString m_displayName;
};

/**
 * The list of tags used to fetch data from the GAL or for a Contact. This list
 * must be kept synchronised with the body of @ref preparePayload.
 *
 * This list is the superset of useful entries from [MS-NSPI] with the address
 * book objects as specified in that function, thus ensuring the best possible
 * unified experience.
 */
static int contactTagList[] = {
	PidTagMessageClass,
	PidTagDisplayName,
	PidTagEmailAddress,
	PidTagAddressType,
	PidTagPrimarySmtpAddress,
	PidTagAccount,

	PidTagObjectType,
	PidTagDisplayType,
	PidTagSurname,
	PidTagGivenName,
	PidTagNickname,
	PidTagDisplayNamePrefix,
	PidTagGeneration,
	PidTagTitle,
	PidTagOfficeLocation,
	PidTagStreetAddress,
	PidTagPostOfficeBox,
	PidTagLocality,
	PidTagStateOrProvince,
	PidTagPostalCode,
	PidTagCountry,
	PidTagLocation,

	PidTagDepartmentName,
	PidTagCompanyName,
	PidTagPostalAddress,

	PidTagHomeAddressStreet,
	PidTagHomeAddressPostOfficeBox,
	PidTagHomeAddressCity,
	PidTagHomeAddressStateOrProvince,
	PidTagHomeAddressPostalCode,
	PidTagHomeAddressCountry,

	PidTagOtherAddressStreet,
	PidTagOtherAddressPostOfficeBox,
	PidTagOtherAddressCity,
	PidTagOtherAddressStateOrProvince,
	PidTagOtherAddressPostalCode,
	PidTagOtherAddressCountry,

	PidTagPrimaryTelephoneNumber,
	PidTagBusinessTelephoneNumber,
	PidTagBusiness2TelephoneNumber,
	PidTagBusiness2TelephoneNumbers,
	PidTagHomeTelephoneNumber,
	PidTagHome2TelephoneNumber,
	PidTagHome2TelephoneNumbers,
	PidTagMobileTelephoneNumber,
	PidTagRadioTelephoneNumber,
	PidTagCarTelephoneNumber,
	PidTagPrimaryFaxNumber,
	PidTagBusinessFaxNumber,
	PidTagHomeFaxNumber,
	PidTagPagerTelephoneNumber,
	PidTagIsdnNumber,

	PidTagGender,
	PidTagPersonalHomePage,
	PidTagBusinessHomePage,
	PidTagBirthday,
	PidTagThumbnailPhoto,
	0 };
static SPropTagArray contactTags = {
	(sizeof(contactTagList) / sizeof(contactTagList[0])) - 1,
	(MAPITAGS *)contactTagList };

/**
 * Take a set of properties, and attempt to apply them to the given addressee.
 * 
 * The switch statement at the heart of this routine must be kept synchronised
 * with @ref contactTagList.
 *
 * @return false on error.
 */
static bool preparePayload(SPropValue *properties, unsigned propertyCount, KABC::Addressee &addressee)
{
	static QString separator = QString::fromAscii(", ");
	unsigned displayType = DT_MAILUSER;
	unsigned objectType = MAPI_MAILUSER;
	QString email;
	QString addressType;
	QString officeLocation;
	QString location;
	KABC::Address postal(KABC::Address::Postal);
	KABC::Address work(KABC::Address::Work);
	KABC::Address home(KABC::Address::Home);
	KABC::Address other(KABC::Address::Pref);

	// Walk through the properties and extract the values of interest. The
	// properties here should be aligned with the list pulled above.
	//
	// We want to decode all the properties in [MS-OXOABK] that we can,
	// subject to the following:
	//
	// - Properties common to all objects (section 2.2.3), and which
	//   apply to both Contacts from [MS-OXOAB] and the GAL from [MS-NSPI].
	//
	// - Properties which apply either to Mail Users (section 2.2.4) or 
	//   Distribution Lists (section 2.2.6) and which map to either 
	//   KABC::Addressee or KABC::DistributionList respectively.
	//
	// TODO For now, we don't do anything useful with distribtion lists.
	for (unsigned i = 0; i < propertyCount; i++) {
		MapiProperty property(properties[i]);

		switch (property.tag()) {
		case PidTagMessageClass:
			// Sanity check the message class.
			if (QLatin1String("IPM.Contact") != property.value().toString()) {
				kError() << "retrieved item is not a contact:" << property.value().toString();
				return false;
			}
			break;
		// 2.2.3.1
		case PidTagDisplayName:
			addressee.setNameFromString(property.value().toString());
			break;
		// 2.2.3.14 and related items.
		case PidTagEmailAddress:
			email = property.value().toString();
			break;
		case PidTagAddressType:
			addressType = property.value().toString();
			break;
		case PidTagPrimarySmtpAddress:
			addressee.setEmails(QStringList(mapiExtractEmail(property, "SMTP")));
			break;
		case PidTagAccount:
			if (!addressee.emails().size()) {
				addressee.insertEmail(mapiExtractEmail(property, "SMTP"));
			}
			break;

		// 2.2.3.10
		case PidTagObjectType:
			objectType = property.value().toUInt();
			break;
		// 2.2.3.11
		case PidTagDisplayType:
			displayType = property.value().toUInt();
			break;
		// 2.2.4.1
		case PidTagSurname:
			addressee.setFamilyName(property.value().toString());
			break;
		// 2.2.4.2
		case PidTagGivenName:
			addressee.setGivenName(property.value().toString());
			break;
		// 2.2.4.3
		case PidTagNickname:
			addressee.setNickName(property.value().toString());
			break;
		// 2.2.4.4
		case PidTagDisplayNamePrefix:
			addressee.setPrefix(property.value().toString());
			break;
		// 2.2.4.6
		case PidTagGeneration:
			addressee.setSuffix(property.value().toString());
			break;
		// 2.2.4.7
		case PidTagTitle:
			addressee.setRole(property.value().toString());
			break;
		// 2.2.4.8 and related items.
		case PidTagOfficeLocation:
			officeLocation = property.value().toString();
			break;
		case PidTagStreetAddress:
			work.setStreet(property.value().toString());
			break;
		case PidTagPostOfficeBox:
			work.setPostOfficeBox(property.value().toString());
			break;
		case PidTagLocality:
			work.setLocality(property.value().toString());
			break;
		case PidTagStateOrProvince:
			work.setRegion(property.value().toString());
			break;
		case PidTagPostalCode:
			work.setPostalCode(property.value().toString());
			break;
		case PidTagCountry:
			work.setCountry(property.value().toString());
			break;
		case PidTagLocation:
			location = property.value().toString();
			break;

		// 2.2.4.9
		case PidTagDepartmentName:
			addressee.setDepartment(property.value().toString());
			break;
		// 2.2.4.10
		case PidTagCompanyName:
			addressee.setOrganization(property.value().toString());
			break;
		// 2.2.4.18
		case PidTagPostalAddress:
			postal.setStreet(property.value().toString());
			break;

		// 2.2.4.25 and related items.
		case PidTagHomeAddressStreet:
			home.setStreet(property.value().toString());
			break;
		case PidTagHomeAddressPostOfficeBox:
			home.setPostOfficeBox(property.value().toString());
			break;
		case PidTagHomeAddressCity:
			home.setLocality(property.value().toString());
			break;
		case PidTagHomeAddressStateOrProvince:
			home.setRegion(property.value().toString());
			break;
		case PidTagHomeAddressPostalCode:
			home.setPostalCode(property.value().toString());
			break;
		case PidTagHomeAddressCountry:
			home.setCountry(property.value().toString());
			break;

		// 2.2.4.31 and related items.
		case PidTagOtherAddressStreet:
			other.setStreet(property.value().toString());
			break;
		case PidTagOtherAddressPostOfficeBox:
			other.setPostOfficeBox(property.value().toString());
			break;
		case PidTagOtherAddressCity:
			other.setLocality(property.value().toString());
			break;
		case PidTagOtherAddressStateOrProvince:
			other.setRegion(property.value().toString());
			break;
		case PidTagOtherAddressPostalCode:
			other.setPostalCode(property.value().toString());
			break;
		case PidTagOtherAddressCountry:
			other.setCountry(property.value().toString());
			break;

		// 2.2.4.37 and related items.
		case PidTagPrimaryTelephoneNumber:
			addressee.insertPhoneNumber(KABC::PhoneNumber(property.value().toString(), KABC::PhoneNumber::Pref | KABC::PhoneNumber::Voice));
			break;
		case PidTagBusinessTelephoneNumber:
		case PidTagBusiness2TelephoneNumber:
		case PidTagBusiness2TelephoneNumbers:
			addressee.insertPhoneNumber(KABC::PhoneNumber(property.value().toString(), KABC::PhoneNumber::Work | KABC::PhoneNumber::Voice));
			break;
		case PidTagHomeTelephoneNumber:
		case PidTagHome2TelephoneNumber:
		case PidTagHome2TelephoneNumbers:
			addressee.insertPhoneNumber(KABC::PhoneNumber(property.value().toString(), KABC::PhoneNumber::Home | KABC::PhoneNumber::Voice));
			break;
		case PidTagMobileTelephoneNumber:
		case PidTagRadioTelephoneNumber:
			addressee.insertPhoneNumber(KABC::PhoneNumber(property.value().toString(), KABC::PhoneNumber::Cell | KABC::PhoneNumber::Voice));
			break;
		case PidTagCarTelephoneNumber:
			addressee.insertPhoneNumber(KABC::PhoneNumber(property.value().toString(), KABC::PhoneNumber::Car | KABC::PhoneNumber::Voice));
			break;
		case PidTagPrimaryFaxNumber:
			addressee.insertPhoneNumber(KABC::PhoneNumber(property.value().toString(), KABC::PhoneNumber::Pref | KABC::PhoneNumber::Fax));
			break;
		case PidTagBusinessFaxNumber:
			addressee.insertPhoneNumber(KABC::PhoneNumber(property.value().toString(), KABC::PhoneNumber::Work | KABC::PhoneNumber::Fax));
			break;
		case PidTagHomeFaxNumber:
			addressee.insertPhoneNumber(KABC::PhoneNumber(property.value().toString(), KABC::PhoneNumber::Home | KABC::PhoneNumber::Fax));
			break;
		case PidTagPagerTelephoneNumber:
			addressee.insertPhoneNumber(KABC::PhoneNumber(property.value().toString(), KABC::PhoneNumber::Pager));
			break;
		case PidTagIsdnNumber:
			addressee.insertPhoneNumber(KABC::PhoneNumber(property.value().toString(), KABC::PhoneNumber::Isdn));
			break;

		// 2.2.4.73
		case PidTagGender:
			switch (property.value().toString().toUInt()) {
			case 1:
				// Female.
				addressee.setTitle(i18n("Ms."));
				break;
			case 2:
				// Male.
				addressee.setTitle(i18n("Mr."));
				break;
			}
			break;
		// 2.2.4.77 and related.
		case PidTagPersonalHomePage:
			addressee.setUrl(KUrl(property.value().toString()));
			break;
		case PidTagBusinessHomePage:
			if (addressee.url().isEmpty()) {
				addressee.setUrl(KUrl(property.value().toString()));
			}
			break;

		// 2.2.4.79
		case PidTagBirthday:
			addressee.setBirthday(property.value().toDateTime());
			break;
		// 2.2.4.82
		case PidTagThumbnailPhoto:
			addressee.setPhoto(KABC::Picture(QImage::fromData(property.value().toByteArray())));
			break;
		default:
			const char *str = get_proptag_name(property.tag());
			QString tagName;

			if (str) {
				tagName = QString::fromAscii(str).mid(6);
			} else {
				tagName = QString::number(property.tag(), 0, 16);
			}

			if (PT_ERROR != (property.tag() & 0xFFFF)) {
				addressee.insertCustom(i18n("Exchange"), tagName, property.toString());
			}

			// Handle oversize objects.
			if (MAPI_E_NOT_ENOUGH_MEMORY == property.value().toInt()) {
				switch (property.tag()) {
				default:
					kError() << "missing oversize support:" << tagName;
					break;
				}

				// Carry on with next property...
				break;
			}
			break;
		}
	}
	if (displayType != DT_MAILUSER) {
		//this->displayType = mapiDisplayType(displayType);
	}
	if (objectType != MAPI_MAILUSER) {
		//this->objectType = mapiObjectType(objectType);
	}

	// Don't override an SMTP address.
	if (!email.isEmpty()) {
		if (!addressee.emails().size()) {
			addressee.insertEmail(mapiExtractEmail(email, addressType.toAscii()));
		}
	}

	// location
	// officeLocation
	// location, officeLocation
	if (!location.isEmpty())
	{
		work.setExtended(location);
	}
	if (!officeLocation.isEmpty()) {
		if (!location.isEmpty())
		{
			work.setExtended(location.append(separator).append(officeLocation));
		} else {
			work.setExtended(officeLocation);
		}
	}

	// Any non-empty addresses?
	if (!postal.formattedAddress().isEmpty()) {
		addressee.insertAddress(postal);
	}
	if (!work.formattedAddress().isEmpty()) {
		addressee.insertAddress(work);
	}
	if (!home.formattedAddress().isEmpty()) {
		addressee.insertAddress(home);
	}
	if (!other.formattedAddress().isEmpty()) {
		addressee.insertAddress(other);
	}
	return true;
}

ExGalResource::ExGalResource(const QString &id) : 
	MapiResource(id, i18n("Exchange Contacts"), IPF_CONTACT, "IPM.Contact", QString::fromAscii("text/directory")),
	m_galId(0, 0),
	m_gal(0),
	m_galUpdater(0)
{
	new SettingsAdaptor(Settings::self());
	QDBusConnection::sessionBus().registerObject(QLatin1String("/Settings"),
						     Settings::self(), 
						     QDBusConnection::ExportAdaptors);
	AttributeFactory::registerAttribute<FetchStatusAttribute>();
}

ExGalResource::~ExGalResource()
{
}

const QString ExGalResource::profile()
{
	// First select who to log in as.
	return Settings::self()->profileName();
}

void ExGalResource::retrieveCollectionAttributes(const Akonadi::Collection &collection)
{
	if (collection.remoteId() == m_galId.toString()) {
		collectionAttributesRetrieved(m_gal);
	} else {
		// We should not get here.
		cancelTask();
	}
}

void ExGalResource::retrieveCollections()
{
	// We are going to return both the user's contacts as well as the GAL.
	// First, the GAL, then the user's contacts...
	Collection::List collections;
	Collection gal;

	qCritical() << "GAL retrieveCollections";
	setName(i18n("Exchange Contacts for %1", profile()));
	gal.setName(i18n("Exchange Global Address List for %1", profile()));
	gal.setRemoteId(m_galId.toString());
	gal.setParentCollection(Collection::root());
	gal.setContentMimeTypes(QStringList(m_itemMimeType));
	gal.setRights(Akonadi::Collection::ReadOnly);
	gal.cachePolicy().setSyncOnDemand(true);
	gal.cachePolicy().setCacheTimeout(MINUTES_IN_ONE_DAY);
	collections.append(gal);
	fetchCollections(Contacts, collections);

	// Notify Akonadi about the new collections.
	collectionsRetrieved(collections);
}
 
void ExGalResource::retrieveItems(const Akonadi::Collection &collection)
{
	Item::List items;
	Item::List deletedItems;

	qCritical() << "GAL retrieveItems" << collection.name();
	if (collection.remoteId() == m_galId.toString()) {
		// Assume the GAL is going to take a while to fetch, so use
		// streaming mode.
		setAutomaticProgressReporting(false);
		setItemStreamingEnabled(true);

		// Now that the collection has come back to us form the backend,
		// it isValid().
		m_gal = collection;
		
		// We are just starting to fetch stuff, see if there is a saved 
		// displayName to start from.
		if (m_gal.hasAttribute(FETCH_STATUS)) {
			// TODO Why is the clone needed?
			FetchStatusAttribute *fetchStatus = static_cast<FetchStatusAttribute *>(m_gal.attribute(FETCH_STATUS)->clone());
			QString savedDisplayName = fetchStatus->displayName();
			
		qCritical() << "GAL retrieveItems task count:" << fetchStatus->dateTime() << fetchStatus->displayName();
			if (!savedDisplayName.isEmpty()) { 
				// Seek to the row at or after the point we remembered.
				emit status(Running, i18n("Seek to GAL at: %1", savedDisplayName));
				qCritical() << "progress:" << __LINE__;
				if (!m_connection->GALSeek(savedDisplayName)) {
					error(i18n("cannot seek to GAL at: %1", savedDisplayName));
					qCritical() << (i18n("cannot seek to GAL at: %1", savedDisplayName));
					return;
				}
			}
			delete fetchStatus;
		} else {
			if (!m_connection->GALRewind()) {
				error(i18n("cannot rewind GAL"));
				qCritical() << (i18n("cannot rewind GAL"));
				return;
			}
		}

		qCritical() << "GAL count" << __LINE__;
		//scheduleCustomTask(this, "retrieveGALBatch", QVariant(), ResourceBase::Append);
		QMetaObject::invokeMethod(this, "retrieveGALBatch", Qt::QueuedConnection);
		cancelTask();
	} else {
		// This request is NOT for the GAL. We don't bother with 
		// streaming mode.
		setAutomaticProgressReporting(true);
		setItemStreamingEnabled(false);
//		fetchItems(collection, items, deletedItems);
		itemsRetrievedIncremental(items, deletedItems);
		itemsRetrievalDone();
		kDebug() << "new/changed items:" << items.size() << "deleted items:" << deletedItems.size();
	}
}

/**
 * Streamed fetch of the GAL, one batch at a time.
 * 
 * Next state: If we have read the entire GAL, @ref updateGALStatus for the 
 * last time, otherwise @ref createGALItem().
 */
//void ExGalResource::retrieveGALBatch(const QVariant &)
void ExGalResource::retrieveGALBatch()
{
	unsigned requestedCount = 300;
	// Actually do the fetching.
	struct SRowSet *results = NULL;
	unsigned percentagePosition;

	// Actually do the fetching.
	emit status(Running, i18n("Reading GAL"));
	qCritical() << "retrieveGALBatch:" << __LINE__;
	if (!m_connection->GALRead(requestedCount, &contactTags, &results, &percentagePosition)) {
		error(i18n("cannot read GAL: %1", mapiError()));
		qCritical() << i18n("cannot read GAL: %1", mapiError());
		return;
	}

	if (!results) {
		// All done!
		emit status(Running, i18n("GAL entries read"));
		qCritical() << "retrieveGALBatch:" << __LINE__;
		updateGALStatus();
		return;
	}
	emit percent(percentagePosition);
		qCritical() << "FetchGALItemsJob:" << __LINE__;

	// For each row, construct an Addressee, and add the item to the list.
	qCritical() << "FetchGALItemsJob:" << __LINE__ << "rows" << results->cRows;
	for (unsigned i = 0; i < results->cRows; i++) {
		struct SRow &contact = results->aRow[i];
		KABC::Addressee addressee;

		if (!preparePayload(contact.lpProps, contact.cValues, addressee)) {
			//emit status(Running, i18n("Skipped malformed GAL entry"));
			continue;
		}
		Item item(m_itemMimeType);
		item.setParentCollection(m_gal);
		item.setRemoteId(addressee.emails()[0]);
		item.setRemoteRevision(QString::number(1));
		item.setPayload<KABC::Addressee>(addressee);

		m_galItems << item;
	}
	MAPIFreeBuffer(results);
	taskDone();
	createGALItem();
}

/**
 * Initiate the creation of a single GAL item by trying to fetch any existing
 * item.
 * 
 * Next state: @ref fetchGALItemDone().
 */
void ExGalResource::createGALItem()
{
	Akonadi::Item item = m_galItems.first();
	Akonadi::ItemFetchJob *job = new Akonadi::ItemFetchJob(item);
	connect(job, SIGNAL(result(KJob *)), SLOT(fetchGALItemDone(KJob *)));
}

/**
 * Initiate the creation of a single GAL item if we didn't find an existing item
 * or else modify the existing one.
 * 
 * Next state: @ref createGALItemDone().
 */
void ExGalResource::fetchGALItemDone(KJob *job)
{
	Akonadi::Item item = m_galItems.first();
	m_galItems.removeFirst();

	// Suppress normal error reporting, since a failed fetch gives us the
	// error "Unknown error. (Item query returned empty result set)".
	//if (job->error()) {
	//	qCritical() << "searchGALItemDone:" << job->errorString();
	//}
	Akonadi::ItemFetchJob *searchJob = qobject_cast<Akonadi::ItemFetchJob *>(job);
	const Akonadi::Item::List contacts = searchJob->items();
	if (contacts.size()) {
		// Update the original item in Akonadi.
		Akonadi::Item originalItem = contacts.first();
		originalItem.setPayload<KABC::Addressee>(item.payload<KABC::Addressee>());
		Akonadi::ItemModifyJob *job = new Akonadi::ItemModifyJob(originalItem);
		connect(job, SIGNAL(result(KJob *)), SLOT(createGALItemDone(KJob *)));
	} else {
		// Save the new item in Akonadi.
		Akonadi::ItemCreateJob *job = new Akonadi::ItemCreateJob(item, m_gal);
		connect(job, SIGNAL(result(KJob *)), SLOT(createGALItemDone(KJob *)));
	}
}
 
/**
 * Complete the creation of a single GAL item.
 * 
 * Next state: If there are more items in the batch, create the next item
 * @ref createGALItem(), otherwise @ref updateGALStatus for the current batch.
 */
void ExGalResource::createGALItemDone(KJob *job)
{
	if (job->error()) {
		qCritical() << "itemCreateJobDone:" << job->errorString();
	}
	if (m_galItems.size()) {
		// Go back and create then next item.
		createGALItem();
	} else if (Akonadi::ItemModifyJob *realJob = dynamic_cast<Akonadi::ItemModifyJob *>(job)) {
		// Update the status of the current batch.
		updateGALStatus(realJob->item().payload<KABC::Addressee>().name());
	} else if (Akonadi::ItemCreateJob *realJob = dynamic_cast<Akonadi::ItemCreateJob *>(job)) {
		// Update the status of the current batch.
		updateGALStatus(realJob->item().payload<KABC::Addressee>().name());
	}
}

/**
 * Initiate update of the fetch status of the GAL, see @ref FetchStatusAttribute.
 * 
 * @param lastAddressee		If not empty, the displayName of the last
 * 				item written. If empty, we will write a final
 * 				timestamp instead.
 * 
 * Next state: @ref updateGALStatusDone().
 */
void ExGalResource::updateGALStatus(QString lastAddressee)
{
	FetchStatusAttribute *fetchStatus = new FetchStatusAttribute();

	if (lastAddressee.isEmpty()) {
		// Set the modified attribute.
		fetchStatus->setDateTime(KDateTime::currentUtcDateTime());
		m_gal.addAttribute(fetchStatus);
	} else {
		// Set the modified attribute.
		fetchStatus->setDisplayName(lastAddressee);
		m_gal.addAttribute(fetchStatus);
	}

	// Push the fetch state out to Akonadi if there is not already a job
	// running for that.
	Akonadi::CollectionAttributesSynchronizationJob *job = new Akonadi::CollectionAttributesSynchronizationJob(m_gal);
	connect(job, SIGNAL(result(KJob *)), SLOT(updateGALStatusDone(KJob *)));
	job->start();
}

/**
 * Complete the update of the fetch status of the GAL.
 * 
 * Next state: Go get the next batch, @ref retrieveGALBatch().
 */
void ExGalResource::updateGALStatusDone(KJob *job)
{
	if (job->error()) {
		qCritical() << "updateGALStatusDone:" << job->errorString();
	}
	qCritical() << "updateGALStatusDone:" << __LINE__;
	taskDone();
	//scheduleCustomTask(this, "retrieveGALBatch", QVariant(), ResourceBase::Append);
	QMetaObject::invokeMethod(this, "retrieveGALBatch", Qt::QueuedConnection);
	qCritical() << "updateGALStatusDone:" << __LINE__;
}

/**
 * Per-item fetch of Contacts.
 */
bool ExGalResource::retrieveItem(const Akonadi::Item &itemOrig, const QSet<QByteArray> &parts)
{
	Q_UNUSED(parts);

	qCritical() << "GAL retrieveItem";
	MapiContact *message = fetchItem<MapiContact>(itemOrig);
	if (!message) {
		return false;
	}

	// Create a clone of the passed in Item and fill it with the payload.
	Akonadi::Item item(itemOrig);
	item.setPayload<KABC::Addressee>(*message);

	// Notify Akonadi about the new data.
	itemRetrieved(item);
	delete message;
	return true;
}

void ExGalResource::aboutToQuit()
{
  // TODO: any cleanup you need to do while there is still an active
  // event loop. The resource will terminate after this method returns
}

void ExGalResource::configure( WId windowId )
{
	ProfileDialog dlgConfig(Settings::self()->profileName());
  	if (windowId)
		KWindowSystem::setMainWindow(&dlgConfig, windowId);

	if (dlgConfig.exec() == KDialog::Accepted) {

		QString profile = dlgConfig.getProfileName();
		Settings::self()->setProfileName( profile );
		Settings::self()->writeConfig();

		synchronize();
		emit configurationDialogAccepted();
	} else {
		emit configurationDialogRejected();
	}
}

void ExGalResource::itemAdded( const Akonadi::Item &item, const Akonadi::Collection &collection )
{
  Q_UNUSED( item );
  Q_UNUSED( collection );

  // TODO: this method is called when somebody else, e.g. a client application,
  // has created an item in a collection managed by your resource.

  // NOTE: There is an equivalent method for collections, but it isn't part
  // of this template code to keep it simple
}

void ExGalResource::itemChanged( const Akonadi::Item &item, const QSet<QByteArray> &parts )
{
  Q_UNUSED( item );
  Q_UNUSED( parts );

  // TODO: this method is called when somebody else, e.g. a client application,
  // has changed an item managed by your resource.

  // NOTE: There is an equivalent method for collections, but it isn't part
  // of this template code to keep it simple
}

void ExGalResource::itemRemoved( const Akonadi::Item &item )
{
  Q_UNUSED( item );

  // TODO: this method is called when somebody else, e.g. a client application,
  // has deleted an item managed by your resource.

  // NOTE: There is an equivalent method for collections, but it isn't part
  // of this template code to keep it simple
}

MapiContact::MapiContact(MapiConnector2 *connector, const char *tallocName, mapi_id_t folderId, mapi_id_t id) :
	MapiMessage(connector, tallocName, folderId, id),
	KABC::Addressee()
{
}

QDebug MapiContact::debug() const
{
	static QString prefix = QString::fromAscii("MapiContact: %1/%2:");
	return MapiObject::debug(prefix.arg(m_folderId, 0, ID_BASE).arg(m_id, 0, ID_BASE)) /*<< title*/;
}

QDebug MapiContact::error() const
{
	static QString prefix = QString::fromAscii("MapiContact: %1/%2:");
	return MapiObject::error(prefix.arg(m_folderId, 0, ID_BASE).arg(m_id, 0, ID_BASE)) /*<< title*/;
}

bool MapiContact::propertiesPull(QVector<int> &tags, const bool tagsAppended, bool pullAll)
{
	if (!tagsAppended) {
		for (unsigned i = 0; i < contactTags.cValues; i++) {
			int newTag = contactTags.aulPropTag[i];
			
			if (!tags.contains(newTag)) {
				tags.append(newTag);
			}
		}
	}
	if (!MapiMessage::propertiesPull(tags, tagsAppended, pullAll)) {
		return false;
	}
	if (!preparePayload(m_properties, m_propertyCount, *this)) {
		return false;
	}
	return true;
}

bool MapiContact::propertiesPull()
{
	static bool tagsAppended = false;
	static QVector<int> tags;

	if (!propertiesPull(tags, tagsAppended, (DEBUG_CONTACT_PROPERTIES) != 0)) {
		tagsAppended = true;
		return false;
	}
	tagsAppended = true;
	return true;
}

AKONADI_RESOURCE_MAIN(ExGalResource)

#include "exgalresource.moc"
