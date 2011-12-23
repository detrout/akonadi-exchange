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

#include <QtDBus/QDBusConnection>

#include <KLocalizedString>
#include <KABC/Address>
#include <KABC/Addressee>
#include <KABC/PhoneNumber>
#include <KABC/Picture>
#include <KWindowSystem>

#include "mapiconnector2.h"
#include "profiledialog.h"

/**
 * Display ids in the same format we use when stored in Akonadi.
 */
#define ID_BASE 36

#ifndef DEBUG_CONTACT_PROPERTIES
#define DEBUG_CONTACT_PROPERTIES 0
#endif

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

	bool propertiesPull(QVector<int> &tags, const bool tagsAppended);
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
				kError() << "retreived item is not a contact:" << property.value().toString();
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
			addressee.insertEmail(property.value().toString(), true);
			break;
		case PidTagAccount:
			addressee.insertEmail(property.value().toString());
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
			if (PT_ERROR != (property.tag() & 0xFFFF)) {
				const char *str = get_proptag_name(property.tag());
				QString tagName;
				if (str) {
					tagName = QString::fromAscii(str).mid(6);
				} else {
					tagName = QString::number(property.tag(), 0, 16);
				}
				addressee.insertCustom(i18n("Exchange"), tagName, property.toString());
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
		addressee.insertEmail(addressType.append(separator).append(email));
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
	m_galId(0, 0)
{
	new SettingsAdaptor(Settings::self());
	QDBusConnection::sessionBus().registerObject(QLatin1String("/Settings"),
						     Settings::self(), 
						     QDBusConnection::ExportAdaptors);
}

ExGalResource::~ExGalResource()
{
}

void ExGalResource::retrieveCollections()
{
	// First select who to log in as.
	profileSet(Settings::self()->profileName());

	// We are going to return both the user's contacts as well as the GAL.
	// First, the GAL, then the user's contacts...
	Collection::List collections;
	Collection gal;

	gal.setName(i18n("Exchange Global Address List"));
	gal.setRemoteId(m_galId.toString());
	gal.setParentCollection(Collection::root());
	gal.setContentMimeTypes(QStringList(m_itemMimeType));
	gal.setRights(Akonadi::Collection::ReadOnly);
	collections.append(gal);
	fetchCollections(Contacts, collections);

	// Notify Akonadi about the new collections.
	collectionsRetrieved(collections);
}

void ExGalResource::retrieveItems(const Akonadi::Collection &collection)
{
	Item::List items;
	Item::List deletedItems;

	if (collection.remoteId() == m_galId.toString()) {
		// Assume the GAL is going to take a while to fetch, so use
		// streaming mode.
		kDebug() << "fetch items from collection:" << collection.name();
		setItemStreamingEnabled(true);
		m_galCollection = collection;
		scheduleCustomTask(this, "retrieveGALItems", QVariant((qulonglong)0), ResourceBase::Append);
		cancelTask();
	} else {
		// This request is NOT for the GAL. We don't bother with 
		// streaming mode.
		fetchItems(collection, items, deletedItems);
		itemsRetrievedIncremental(items, deletedItems);
		kDebug() << "new/changed items:" << items.size() << "deleted items:" << deletedItems.size();
	}
}

/**
 * Streamed fetch of the GAL.
 */
void ExGalResource::retrieveGALItems(const QVariant &countVariant)
{
	qulonglong count = countVariant.toULongLong();
	unsigned requestedCount = 300;
	Item::List items;
	Item::List deletedItems;
	struct SRowSet *results = NULL;

	emit status(Running, i18n("Fetching GAL from Exchange"));
	if (!m_connection->fetchGAL(count == 0, requestedCount, &contactTags, &results)) {
		error(i18n("cannot fetch GAL from Exchange"));
		return;
	}
	if (!results) {
		// All done!
		emit status(Running, i18n("%1 GAL entries returned from Exchange", count));
		itemsRetrievalDone();
		return;
	}

	// For each row, construct an Addressee, and add the item to the list.
	for (unsigned i = 0; i < results->cRows; i++) {
		struct SRow &contact = results->aRow[i];
		KABC::Addressee addressee;

		if (!preparePayload(contact.lpProps, contact.cValues, addressee)) {
			emit status(Running, i18n("Skipped malformed GAL entry"));
			continue;
		}
		Item item(m_itemMimeType);
		item.setParentCollection(m_galCollection);
		item.setRemoteId(addressee.emails()[0]);
		item.setRemoteRevision(QString::number(1));
		item.setPayload<KABC::Addressee>(addressee);

		items << item;
	}
	MAPIFreeBuffer(results);
	count += items.size();
	itemsRetrievedIncremental(items, deletedItems);
#if (DEBUG_CONTACT_PROPERTIES)
	// Exit early for debug only.
	if (count > 3 * requestedCount) {
		requestedCount = items.size() + 1;
	}
#endif
	if ((unsigned)items.size() < requestedCount) {
		// All done!
		emit status(Running, i18n("%1 GAL entries returned from Exchange", count));
		itemsRetrievalDone();
	} else {
		// Go around again for more...
		scheduleCustomTask(this, "retrieveGALItems", QVariant(count), ResourceBase::Append);
	}
	// Uncommenting this causes retrieveItem() to fail for Contacts...
//	taskDone();
}

/**
 * Per-item fetch of Contacts.
 */
bool ExGalResource::retrieveItem(const Akonadi::Item &itemOrig, const QSet<QByteArray> &parts)
{
	Q_UNUSED(parts);

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

bool MapiContact::propertiesPull(QVector<int> &tags, const bool tagsAppended)
{
	if (!tagsAppended) {
		for (unsigned i = 0; i < contactTags.cValues; i++) {
			int newTag = contactTags.aulPropTag[i];
			
			if (!tags.contains(newTag)) {
				tags.append(newTag);
			}
		}
	}
	if (!MapiMessage::propertiesPull(tags, tagsAppended)) {
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

	if (!propertiesPull(tags, tagsAppended)) {
		tagsAppended = true;
		return false;
	}
	tagsAppended = true;
	return true;
}

AKONADI_RESOURCE_MAIN(ExGalResource)

#include "exgalresource.moc"
